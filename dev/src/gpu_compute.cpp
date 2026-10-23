#include "gpu_compute.hpp"
#include "gpu_kernels.hpp"
#include "perceptual_color.hpp"
#include "wavelet.hpp"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <mutex>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <atomic>
#include <bit>
#include <cmath>
namespace fajk {
namespace {
using Microsoft::WRL::ComPtr;
thread_local ComputeBackend selectedBackend=ComputeBackend::Cpu;
thread_local uint64_t jobStamp=0;
std::atomic<uint64_t> nextStamp{0};
thread_local GpuActivity activity;
void checked(HRESULT hr,const char* operation){
    if(FAILED(hr)){std::ostringstream s;s<<operation<<" (0x"<<std::hex<<uint32_t(hr)<<")";throw std::runtime_error(s.str());}
}
struct Buffer {
    ComPtr<ID3D11Buffer> resource;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    uint32_t bytes=0;
};
struct Parameters {uint32_t plane[3][4]{},level=0,inverse=0,count=0,unused=0,image[4]{},restoration[4]{},options[4]{};};
struct Tap {uint32_t first,count;};
struct Weight {uint32_t index;float value;};
struct SourceSlot {
    Buffer buffer;
    const uint8_t* pointer=nullptr;
    uint64_t stamp=0,touched=0;
};
struct ColorSourceSlot {Buffer buffer;const uint8_t* pointer=nullptr;uint64_t stamp=0,touched=0;uint32_t width=0,height=0;};
struct Engine {
    std::mutex mutex;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIDevice2> dxgiDevice;
    HANDLE ready=nullptr;
    ComPtr<ID3D11Buffer> constants;
    std::array<ComPtr<ID3D11ComputeShader>,gpuKernels.size()> shaders;
    Buffer waveA,waveB,waveRead,contourResult;
    std::array<ColorSourceSlot,2> colorSources;
    SourceSlot currentSource;
    std::array<SourceSlot,3> sourceCache;
    Buffer& rgba=currentSource.buffer;
    uint64_t sourceClock=0;
    Buffer temp,out,outRead,tapBuffer,weightBuffer,upscaleAxes;
    Buffer stats,statsRead,gamma,slopes,means,guides,linear,colorStats,colorStatsRead,colorPatches,colorPatchesRead,colorMoments;
    uint32_t iw=0,ih=0,ow=0,oh=0;
    GpuInfo info;
    bool attempted=false;
    bool doubles=false;
    ~Engine(){if(ready)CloseHandle(ready);}
    void initialize(){
        if(attempted){if(!info.available)throw std::runtime_error(info.error);return;}
        attempted=true;
        ComPtr<IDXGIFactory1> factory;checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create GPU factory");
        ComPtr<IDXGIAdapter1> adapter,best;SIZE_T memory=0;
        for(UINT i=0;factory->EnumAdapters1(i,&adapter)==S_OK;++i){DXGI_ADAPTER_DESC1 d{};adapter->GetDesc1(&d);
            if(!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&(!best||d.DedicatedVideoMemory>memory)){best=adapter;memory=d.DedicatedVideoMemory;}}
        if(!best)throw std::runtime_error("No hardware Direct3D adapter.");
        DXGI_ADAPTER_DESC1 desc{};best->GetDesc1(&desc);
        const int n=WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,nullptr,0,nullptr,nullptr);
        std::string name(size_t(n),0);WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name.data(),n,nullptr,nullptr);
        if(!name.empty())name.pop_back();info.device=std::move(name);
        D3D_FEATURE_LEVEL level;const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        checked(D3D11CreateDevice(best.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels,1,D3D11_SDK_VERSION,&device,&level,&context),"Create GPU device");
        if(SUCCEEDED(device.As(&dxgiDevice)))ready=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        D3D11_FEATURE_DATA_DOUBLES doubleSupport{};
        doubles=SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_DOUBLES,&doubleSupport,sizeof(doubleSupport)))&&doubleSupport.DoublePrecisionFloatShaderOps;
        for(size_t i=0;i<gpuKernels.size();++i){
            if(i>=19&&i<=21&&!doubles)continue;
            checked(device->CreateComputeShader(gpuKernels[i].bytes,gpuKernels[i].size,nullptr,&shaders[i]),"Create R470 GPU kernel");
        }
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Parameters);cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        checked(device->CreateBuffer(&cb,nullptr,&constants),"Create GPU parameters");info.available=true;info.error.clear();
    }
    void ensure(Buffer& b,uint32_t count,uint32_t stride,bool writable=false,bool readback=false){
        const uint64_t bytes=uint64_t(count)*stride;
        if(!bytes||bytes>256ull*1024*1024)throw std::runtime_error("GPU working buffer exceeds 256 MiB safety limit.");
        if(b.bytes==bytes&&b.resource&&(readback?!b.srv:bool(b.srv))&&(!writable||b.uav))return;
        if(&b==&rgba){currentSource.stamp=0;currentSource.pointer=nullptr;}
        b={};D3D11_BUFFER_DESC d{};d.ByteWidth=uint32_t(bytes);
        d.Usage=readback?D3D11_USAGE_STAGING:D3D11_USAGE_DEFAULT;
        d.BindFlags=readback?0:D3D11_BIND_SHADER_RESOURCE|(writable?D3D11_BIND_UNORDERED_ACCESS:0);
        d.CPUAccessFlags=readback?D3D11_CPU_ACCESS_READ:0;
        d.MiscFlags=readback?0:D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=readback?0:stride;
        checked(device->CreateBuffer(&d,nullptr,&b.resource),"Allocate GPU buffer");
        if(!readback){checked(device->CreateShaderResourceView(b.resource.Get(),nullptr,&b.srv),"Create GPU input");
            if(writable)checked(device->CreateUnorderedAccessView(b.resource.Get(),nullptr,&b.uav),"Create GPU output");}
        b.bytes=uint32_t(bytes);
    }
    void parameters(const Parameters& p){context->UpdateSubresource(constants.Get(),0,nullptr,&p,0,0);auto* cb=constants.Get();context->CSSetConstantBuffers(0,1,&cb);}
    void clearBindings(){if(!context)return;ID3D11ShaderResourceView* s[9]{};ID3D11UnorderedAccessView* u[5]{};
        context->CSSetShaderResources(0,9,s);context->CSSetUnorderedAccessViews(0,5,u,nullptr);}
    void upload(Buffer& b,const void* data,uint32_t bytes,uint32_t offset=0){
        D3D11_BOX box{offset,0,0,offset+bytes,1,1};context->UpdateSubresource(b.resource.Get(),0,&box,data,0,0);activity.uploadedBytes+=bytes;
    }
    bool activateSource(uint32_t bytes,const uint8_t* pointer,uint64_t stamp){
        // Two overlapping frames each have original and working-size RGBA.
        // Retain at most four <=32 MiB buffers; large images keep one buffer.
        // Keys include the immutable job stamp, never a recycled address alone.
        const auto matches=[&](const SourceSlot& s){return stamp&&s.stamp==stamp&&s.pointer==pointer&&s.buffer.bytes==bytes&&s.buffer.resource;};
        if(matches(currentSource)){currentSource.touched=++sourceClock;return true;}
        for(auto& slot:sourceCache)if(matches(slot)){std::swap(currentSource,slot);currentSource.touched=++sourceClock;return true;}
        constexpr uint32_t cacheLimit=32u*1024*1024;
        if(bytes>cacheLimit){for(auto& slot:sourceCache)slot={};}
        else{
            if(rgba.bytes>cacheLimit)currentSource={};
            if(currentSource.stamp||rgba.bytes!=bytes){
                // Prefer a reusable allocation of the correct size, then LRU.
                auto* chosen=&sourceCache[0];
                for(auto& slot:sourceCache){const bool same=slot.buffer.bytes==bytes,previous=chosen->buffer.bytes==bytes;
                    if((same&&!previous)||(same==previous&&slot.touched<chosen->touched))chosen=&slot;}
                std::swap(currentSource,*chosen);
            }
        }
        ensure(rgba,bytes/4,4);currentSource.stamp=0;currentSource.pointer=nullptr;currentSource.touched=++sourceClock;return false;
    }
    void uploadSource(const Frame& source){
        if(activateSource(uint32_t(source.pixels.size()),source.pixels.data(),jobStamp))return;
        upload(rgba,source.pixels.data(),uint32_t(source.pixels.size()));
        currentSource.pointer=source.pixels.data();currentSource.stamp=jobStamp;
    }
    void retainResampled(std::span<const uint8_t> pixels){
        // The host still needs these bytes, but the following GPU color and
        // source analysis can consume the existing packed result directly.
        // A source job stamp, not a pointer alone, defines immutability.
        if(!jobStamp||pixels.size()>32u*1024*1024)return;
        auto* chosen=&sourceCache[0];
        for(auto& slot:sourceCache)if(slot.touched<chosen->touched)chosen=&slot;
        std::swap(chosen->buffer,out);
        chosen->pointer=pixels.data();chosen->stamp=jobStamp;chosen->touched=++sourceClock;
    }
    void waitForGpu(){
        // A blocking Map can spin inside the driver. Sleep on the GPU completion
        // event instead, so offloading also releases CPU cores to other work.
        if(ready&&dxgiDevice){context->Flush();checked(dxgiDevice->EnqueueSetEvent(ready),"Schedule GPU completion event");
            if(WaitForSingleObject(ready,10000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU completion timeout.");}
    }
    void fail(const std::exception& e){clearBindings();++activity.fallbacks;info.error=e.what();
        // Geometry/resource-limit errors may recover on the next image. A removed
        // device or failed initialization remains unavailable, without retry loops.
        if(!device||FAILED(device->GetDeviceRemovedReason())||!shaders.front())info.available=false;
    }
    void transform(std::span<GpuPlane> planes,bool inverse,int first,int levels,bool download=true,uint32_t cleanup=0,bool uploaded=false){
        initialize();Parameters p{};p.options[0]=cleanup;p.count=uint32_t(planes.size());p.inverse=inverse;uint32_t total=0;
        for(size_t c=0;c<planes.size();++c){const auto& plane=planes[c];p.plane[c][0]=plane.width;p.plane[c][1]=plane.height;p.plane[c][2]=total;total+=uint32_t(plane.pixels.size());}
        ensure(waveA,total,4,true);ensure(waveB,total,4,true);if(download)ensure(waveRead,total,4,false,true);
        if(!uploaded)for(size_t c=0;c<planes.size();++c)upload(waveA,planes[c].pixels.data(),uint32_t(planes[c].pixels.size_bytes()),p.plane[c][2]*4);
        auto dispatch=[&](bool vertical,Buffer& input,Buffer& output){
            auto* srv=input.srv.Get();auto* uav=output.uav.Get();context->CSSetShaderResources(0,1,&srv);context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
            context->CSSetShader(shaders[(inverse?12:0)+(vertical?1:0)].Get(),nullptr,0);
            const uint32_t w=planes[0].width>>p.level,h=planes[0].height>>p.level;
            context->Dispatch(vertical?(w+15)/16:(w/2+63)/64,vertical?(h/2+63)/64:(h+15)/16,p.count);clearBindings();
        };
        for(int i=0;i<levels;++i){p.level=inverse?first+levels-1-i:first+i;parameters(p);
            dispatch(inverse,waveA,waveB);dispatch(!inverse,waveB,waveA);
            if(inverse&&p.level==1&&cleanup){
                for(int axis=0;axis<2;++axis){Buffer& a=axis?waveB:waveA;Buffer& b=axis?waveA:waveB;
                    auto* srv=a.srv.Get();auto* uav=b.uav.Get();context->CSSetShaderResources(0,1,&srv);context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);context->CSSetShader(shaders[9+axis].Get(),nullptr,0);
                    context->Dispatch(((axis?planes[0].width:planes[0].height)/2+63)/64,1,cleanup>=8?2:1);clearBindings();}
            }
        }
        ++activity.transforms;if(!download)return;
        context->CopyResource(waveRead.resource.Get(),waveA.resource.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};waitForGpu();checked(context->Map(waveRead.resource.Get(),0,D3D11_MAP_READ,0,&mapped),"Read GPU transform");
        for(size_t c=0;c<planes.size();++c)std::memcpy(planes[c].pixels.data(),static_cast<const float*>(mapped.pData)+p.plane[c][2],planes[c].pixels.size_bytes());
        context->Unmap(waveRead.resource.Get(),0);activity.downloadedBytes+=uint64_t(total)*4;
    }
    void sourceColor(const Frame& source,std::span<GpuPlane> planes,uint32_t tile=0,std::span<GpuColorStatistics> fitted={},std::span<ColorModelPatch> patches={}){
        initialize();Parameters p{};p.image[0]=source.width;p.image[1]=source.height;uint32_t total=0;
        for(size_t c=0;c<3;++c){p.plane[c][0]=planes[c].width;p.plane[c][1]=planes[c].height;p.plane[c][2]=total;total+=uint32_t(planes[c].pixels.size());}
        ensure(waveA,total,4,true);ensure(waveRead,total,4,false,true);
        if(!linear.resource){const auto& table=perceptualColor::linearTable();ensure(linear,uint32_t(table.size()),4);upload(linear,table.data(),linear.bytes);}
        uploadSource(source);parameters(p);
        auto* rgb=rgba.srv.Get();auto* table=linear.srv.Get();auto* target=waveA.uav.Get();context->CSSetShaderResources(1,1,&rgb);context->CSSetShaderResources(5,1,&table);context->CSSetUnorderedAccessViews(0,1,&target,nullptr);
        context->CSSetShader(shaders[8].Get(),nullptr,0);context->Dispatch((planes[0].width+15)/16,(planes[0].height+15)/16,1);clearBindings();
        if(!fitted.empty()){
            static_assert(sizeof(GpuColorStatistics)==80);
            ensure(colorStats,uint32_t(fitted.size()),sizeof(GpuColorStatistics),true);
            ensure(colorStatsRead,uint32_t(fitted.size()),sizeof(GpuColorStatistics),false,true);
            p.restoration[0]=tile;p.restoration[2]=(source.width+tile-1)/tile;
            p.restoration[3]=(source.height+tile-1)/tile;parameters(p);
            auto* colors=waveA.srv.Get();auto* statistics=colorStats.uav.Get();
            context->CSSetShaderResources(0,1,&colors);context->CSSetUnorderedAccessViews(3,1,&statistics,nullptr);
            context->CSSetShader(shaders[19].Get(),nullptr,0);context->Dispatch(uint32_t(fitted.size()),1,1);clearBindings();
            context->CopyResource(colorStatsRead.resource.Get(),colorStats.resource.Get());
            if(!patches.empty()){
                const uint32_t mw=(source.width+1)/2,mh=(source.height+1)/2;
                ensure(colorMoments,mw*mh,32,true);
                auto* moment=colorMoments.uav.Get();context->CSSetShaderResources(0,1,&colors);
                context->CSSetUnorderedAccessViews(1,1,&moment,nullptr);context->CSSetShader(shaders[21].Get(),nullptr,0);
                context->Dispatch((mw+15)/16,(mh+15)/16,1);clearBindings();
                ensure(colorPatches,uint32_t(patches.size()),sizeof(ColorModelPatch),true);
                ensure(colorPatchesRead,uint32_t(patches.size()),sizeof(ColorModelPatch),false,true);
                auto* result=colorPatches.uav.Get();context->CSSetShaderResources(0,1,&colors);
                auto* moments=colorMoments.srv.Get();context->CSSetShaderResources(4,1,&moments);
                context->CSSetUnorderedAccessViews(3,1,&result,nullptr);context->CSSetShader(shaders[20].Get(),nullptr,0);
                context->Dispatch(uint32_t(patches.size()),1,1);clearBindings();
                context->CopyResource(colorPatchesRead.resource.Get(),colorPatches.resource.Get());
            }
        }
        context->CopyResource(waveRead.resource.Get(),waveA.resource.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        waitForGpu();checked(context->Map(waveRead.resource.Get(),0,D3D11_MAP_READ,0,&mapped),"Read GPU source color");
        for(size_t c=0;c<3;++c)std::memcpy(planes[c].pixels.data(),static_cast<const float*>(mapped.pData)+p.plane[c][2],planes[c].pixels.size_bytes());
        context->Unmap(waveRead.resource.Get(),0);activity.downloadedBytes+=uint64_t(total)*4;
        if(!fitted.empty()){
            checked(context->Map(colorStatsRead.resource.Get(),0,D3D11_MAP_READ,0,&mapped),"Read GPU color statistics");
            std::memcpy(fitted.data(),mapped.pData,fitted.size_bytes());context->Unmap(colorStatsRead.resource.Get(),0);
            activity.downloadedBytes+=fitted.size_bytes();++activity.analyses;
            if(!patches.empty()){
                checked(context->Map(colorPatchesRead.resource.Get(),0,D3D11_MAP_READ,0,&mapped),"Read GPU color patch statistics");
                std::memcpy(patches.data(),mapped.pData,patches.size_bytes());context->Unmap(colorPatchesRead.resource.Get(),0);
                activity.downloadedBytes+=patches.size_bytes();
            }
        }
    }
    void retainSourceColor(const Frame& source,std::span<GpuPlane> planes){
        if(!jobStamp||uint64_t(planes[0].pixels.size())*12>96ull*1024*1024)return;
        auto* slot=&colorSources[0];
        for(auto& value:colorSources)if(value.touched<slot->touched)slot=&value;
        std::swap(slot->buffer,waveA);slot->pointer=source.pixels.data();slot->stamp=jobStamp;
        slot->width=planes[0].width;slot->height=planes[0].height;slot->touched=++sourceClock;
    }
    bool sourceWavelet(const Frame& source,std::span<GpuPlane> planes,uint32_t tile,
                       std::span<const uint8_t> map,std::span<const float> cellMeans){
        ColorSourceSlot* resident=nullptr;
        for(auto& slot:colorSources)if(slot.stamp==jobStamp&&slot.pointer==source.pixels.data()&&
            slot.width==planes[0].width&&slot.height==planes[0].height&&slot.buffer.resource)resident=&slot;
        if(!resident)return false;
        Parameters p{};uint32_t total=0;
        for(size_t c=0;c<3;++c){p.plane[c][0]=planes[c].width;p.plane[c][1]=planes[c].height;p.plane[c][2]=total;total+=uint32_t(planes[c].pixels.size());}
        p.image[0]=source.width;p.image[1]=source.height;
        p.restoration[1]=tile;p.restoration[2]=(source.width+tile-1)/tile;p.restoration[3]=(source.height+tile-1)/tile;
        ensure(slopes,uint32_t(cellMeans.size()),4);ensure(means,uint32_t(cellMeans.size()),4,true);
        std::vector<uint32_t> packedMap(cellMeans.size());
        for(size_t i=0;i<packedMap.size();++i)packedMap[i]=map[i*2]|(uint32_t(map[i*2+1])<<8);
        upload(slopes,packedMap.data(),slopes.bytes);upload(means,cellMeans.data(),means.bytes);
        ensure(waveA,total,4,true);parameters(p);
        ID3D11ShaderResourceView* srvs[8]{resident->buffer.srv.Get(),nullptr,nullptr,nullptr,nullptr,nullptr,slopes.srv.Get(),means.srv.Get()};
        auto* target=waveA.uav.Get();context->CSSetShaderResources(0,8,srvs);context->CSSetUnorderedAccessViews(0,1,&target,nullptr);
        context->CSSetShader(shaders[22].Get(),nullptr,0);context->Dispatch((planes[0].width+15)/16,(planes[0].height+15)/16,1);clearBindings();
        // Padding is a multiple of 64: the first three horizontal levels
        // have no scalar normalization tails, including odd source sizes.
        // The remaining 1/64 of the image keeps CPU division semantics.
        transform(planes,false,0,3,true,0,true);resident->touched=++sourceClock;return true;
    }
    void edgeUpscale(const Frame& source,uint32_t width,uint32_t height,std::span<uint8_t> target,
                     std::span<const int> xLow,std::span<const float> xFraction,
                     std::span<const int> yLow,std::span<const float> yFraction,float sharpness,Buffer* resident=nullptr){
        initialize();
        struct Axis {int low;float fraction;};
        std::vector<Axis> axes(size_t(width)+height);
        for(uint32_t i=0;i<width;++i)axes[i]={xLow[i],xFraction[i]};
        for(uint32_t i=0;i<height;++i)axes[width+i]={yLow[i],yFraction[i]};
        ensure(upscaleAxes,uint32_t(axes.size()),sizeof(Axis));
        ensure(out,width*height,4,true);ensure(outRead,width*height,4,false,true);
        // CPU cleanup/text restoration has changed these pixels. Never reuse
        // an immutable encoder source entry based on the recycled address.
        if(!resident){
            activateSource(uint32_t(source.pixels.size()),source.pixels.data(),0);
            upload(rgba,source.pixels.data(),uint32_t(source.pixels.size()));
        }
        upload(upscaleAxes,axes.data(),upscaleAxes.bytes);
        Parameters p{};p.plane[0][0]=source.width;p.plane[0][1]=source.height;
        p.image[0]=width;p.image[1]=height;p.restoration[0]=std::bit_cast<uint32_t>(sharpness);parameters(p);
        auto* raw=(resident?resident:&rgba)->srv.Get();auto* axis=upscaleAxes.srv.Get();auto* result=out.uav.Get();
        context->CSSetShaderResources(1,1,&raw);context->CSSetShaderResources(2,1,&axis);
        context->CSSetUnorderedAccessViews(2,1,&result,nullptr);context->CSSetShader(shaders[18].Get(),nullptr,0);
        context->Dispatch((width+31)/32,(height+7)/8,1);clearBindings();
        context->CopyResource(outRead.resource.Get(),out.resource.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        waitForGpu();checked(context->Map(outRead.resource.Get(),0,D3D11_MAP_READ,0,&mapped),"Read GPU edge upscale");
        std::memcpy(target.data(),mapped.pData,target.size());context->Unmap(outRead.resource.Get(),0);
        activity.downloadedBytes+=target.size();++activity.resamples;
    }
    void finalSharpen(std::span<uint8_t> target,uint32_t width,uint32_t height,uint32_t strength){
        initialize();activateSource(uint32_t(target.size()),target.data(),0);upload(rgba,target.data(),uint32_t(target.size()));
        ensure(out,width*height,4,true);ensure(outRead,width*height,4,false,true);
        Parameters p{};p.image[0]=width;p.image[1]=height;p.image[2]=strength;parameters(p);
        auto* raw=rgba.srv.Get();auto* result=out.uav.Get();context->CSSetShaderResources(1,1,&raw);context->CSSetUnorderedAccessViews(2,1,&result,nullptr);
        context->CSSetShader(shaders[23].Get(),nullptr,0);context->Dispatch((width+15)/16,(height+15)/16,1);clearBindings();
        context->CopyResource(outRead.resource.Get(),out.resource.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        waitForGpu();checked(context->Map(outRead.resource.Get(),0,D3D11_MAP_READ,0,&mapped),"Read GPU final sharpening");
        std::memcpy(target.data(),mapped.pData,target.size());context->Unmap(outRead.resource.Get(),0);
        activity.downloadedBytes+=target.size();++activity.sharpens;
    }
    void sourceStatistics(const Frame& source,uint32_t cell,int mode,std::span<GpuSourceStatistics> output){
        initialize();
        ensure(stats,uint32_t(output.size()),sizeof(GpuSourceStatistics),true);ensure(statsRead,uint32_t(output.size()),sizeof(GpuSourceStatistics),false,true);
        uploadSource(source);Parameters p{};
        p.image[0]=source.width;p.image[1]=source.height;p.level=uint32_t(mode);p.restoration[0]=cell;
        p.restoration[2]=(source.width+cell-1)/cell;p.restoration[3]=(source.height+cell-1)/cell;parameters(p);
        auto* srv=rgba.srv.Get();auto* uav=stats.uav.Get();context->CSSetShaderResources(1,1,&srv);context->CSSetUnorderedAccessViews(3,1,&uav,nullptr);
        context->CSSetShader(shaders[4].Get(),nullptr,0);context->Dispatch((p.restoration[2]+7)/8,(p.restoration[3]+7)/8,1);clearBindings();
        context->CopyResource(statsRead.resource.Get(),stats.resource.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        waitForGpu();checked(context->Map(statsRead.resource.Get(),0,D3D11_MAP_READ,0,&mapped),"Read GPU source statistics");
        std::memcpy(output.data(),mapped.pData,output.size_bytes());context->Unmap(statsRead.resource.Get(),0);
        activity.downloadedBytes+=output.size_bytes();++activity.analyses;
    }
    void decodeNative(std::span<GpuPlane> planes,const GpuNativeSettings& settings,std::span<uint8_t> output){
        // One upload of coefficient planes. All levels, CfL means, guided chroma
        // and Oklab->RGBA stay resident; only packed RGBA is copied back.
        transform(planes,true,0,6,false,settings.cleanup);Parameters p{};uint32_t offset=0;
        for(size_t c=0;c<planes.size();++c){p.plane[c][0]=planes[c].width;p.plane[c][1]=planes[c].height;p.plane[c][2]=offset;offset+=uint32_t(planes[c].pixels.size());}
        p.image[0]=settings.width;p.image[1]=settings.height;p.image[2]=settings.chroma420?1:settings.chroma422?2:0;p.image[3]=settings.smoothing;
        p.restoration[0]=settings.guide;
        if(!settings.cflMap.empty()){p.restoration[1]=settings.tile;p.restoration[2]=(settings.width+settings.tile-1)/settings.tile;p.restoration[3]=(settings.height+settings.tile-1)/settings.tile;}
        p.options[0]=settings.cleanup;p.options[1]=settings.strength;p.options[2]=settings.lumaStrength;p.options[3]=settings.filterPlanes;
        parameters(p);
        if(settings.strength){
            auto* srv=waveA.srv.Get();auto* uav=waveB.uav.Get();context->CSSetShaderResources(0,1,&srv);context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);context->CSSetShader(shaders[11].Get(),nullptr,0);
            context->Dispatch((planes[0].width+7)/8,(planes[0].height+7)/8,3);clearBindings();std::swap(waveA,waveB);
        }
        ensure(out,settings.width*settings.height,4,true);ensure(outRead,settings.width*settings.height,4,false,true);
        if(!gamma.resource){const auto& table=perceptualColor::gammaTable();ensure(gamma,uint32_t(table.size()),4);upload(gamma,table.data(),gamma.bytes);}
        if(!settings.cflMap.empty()){
            const uint32_t cells=p.restoration[2]*p.restoration[3];ensure(slopes,cells,4);ensure(means,cells,4,true);
            std::vector<uint32_t> map(cells);for(size_t i=0;i<cells;++i)map[i]=settings.cflMap[i*2]|(uint32_t(settings.cflMap[i*2+1])<<8);
            upload(slopes,map.data(),slopes.bytes);auto* srv=waveA.srv.Get();auto* uav=means.uav.Get();
            context->CSSetShaderResources(0,1,&srv);context->CSSetUnorderedAccessViews(3,1,&uav,nullptr);context->CSSetShader(shaders[5].Get(),nullptr,0);
            context->Dispatch(p.restoration[2],p.restoration[3],1);clearBindings();
        }
        if(settings.chroma420&&settings.guide){ensure(guides,planes[1].width*planes[1].height,4,true);
            auto* srv=waveA.srv.Get();auto* uav=guides.uav.Get();context->CSSetShaderResources(0,1,&srv);context->CSSetUnorderedAccessViews(4,1,&uav,nullptr);context->CSSetShader(shaders[6].Get(),nullptr,0);
            context->Dispatch(((settings.width+1)/2+15)/16,((settings.height+1)/2+15)/16,1);clearBindings();}
        ID3D11ShaderResourceView* srvs[9]{waveA.srv.Get(),nullptr,nullptr,nullptr,nullptr,gamma.srv.Get(),slopes.srv.Get(),means.srv.Get(),guides.srv.Get()};
        auto* uav=out.uav.Get();context->CSSetShaderResources(0,9,srvs);context->CSSetUnorderedAccessViews(2,1,&uav,nullptr);context->CSSetShader(shaders[7].Get(),nullptr,0);
        context->Dispatch((settings.width+15)/16,(settings.height+15)/16,1);clearBindings();
        context->CopyResource(outRead.resource.Get(),out.resource.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        waitForGpu();checked(context->Map(outRead.resource.Get(),0,D3D11_MAP_READ,0,&mapped),"Read GPU native decode");std::memcpy(output.data(),mapped.pData,output.size());context->Unmap(outRead.resource.Get(),0);
        activity.downloadedBytes+=output.size();++activity.nativeDecodes;
    }

    void contourCleanup(std::span<uint8_t> target,std::span<const uint8_t> source,uint32_t width,uint32_t height,
                        float amount,float stronger,float curvature,std::span<const uint8_t> skipTiles,bool fused,bool retain=false){
        initialize();const uint32_t columns=(width+95)/96,rows=(height+95)/96;
        const bool weak=amount<=.2f&&stronger==0&&curvature<=.06f;
        activateSource(uint32_t(source.size()),source.data(),0);ensure(out,width*height,4,true);
        if(!retain)ensure(outRead,width*height,4,false,true);
        // Packed bytes only cross PCIe; recursive float intermediates remain on
        // the device. Do not reuse the immutable encode source cache here.
        upload(rgba,source.data(),uint32_t(source.size()));
        Parameters p{};p.image[0]=width;p.image[1]=height;
        p.restoration[0]=std::bit_cast<uint32_t>(amount);p.restoration[1]=std::bit_cast<uint32_t>(stronger);
        p.restoration[2]=std::bit_cast<uint32_t>(curvature);p.restoration[3]=std::bit_cast<uint32_t>(1.f/(32+stronger*20));
        p.options[1]=columns;p.options[2]=fused;p.options[3]=!skipTiles.empty();parameters(p);
        if(!weak){
            ensure(temp,columns*rows*12544,16,true);
            if(!skipTiles.empty()){std::vector<uint32_t> mask(skipTiles.begin(),skipTiles.end());ensure(slopes,uint32_t(mask.size()),4);upload(slopes,mask.data(),slopes.bytes);}
            for(uint32_t shader:{14u,15u}){
                auto* raw=rgba.srv.Get();auto* mask=skipTiles.empty()?nullptr:slopes.srv.Get();auto* scratch=temp.uav.Get();
                context->CSSetShaderResources(1,1,&raw);context->CSSetShaderResources(6,1,&mask);context->CSSetUnorderedAccessViews(1,1,&scratch,nullptr);
                context->CSSetShader(shaders[shader].Get(),nullptr,0);context->Dispatch(columns*rows,7,1);clearBindings();
            }
        }
        auto* raw=rgba.srv.Get();auto* scratch=weak?nullptr:temp.srv.Get();auto* mask=skipTiles.empty()?nullptr:slopes.srv.Get();auto* result=out.uav.Get();
        context->CSSetShaderResources(1,1,&raw);context->CSSetShaderResources(4,1,&scratch);context->CSSetShaderResources(6,1,&mask);context->CSSetUnorderedAccessViews(2,1,&result,nullptr);
        context->CSSetShader(shaders[weak?17:16].Get(),nullptr,0);context->Dispatch((width+15)/16,(height+15)/16,1);clearBindings();
        if(retain){std::swap(contourResult,out);++activity.contourFilters;return;}
        context->CopyResource(outRead.resource.Get(),out.resource.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        waitForGpu();checked(context->Map(outRead.resource.Get(),0,D3D11_MAP_READ,0,&mapped),"Read GPU contour cleanup");
        std::memcpy(target.data(),mapped.pData,target.size());context->Unmap(outRead.resource.Get(),0);
        activity.downloadedBytes+=target.size();++activity.contourFilters;
    }
    void resample(const Frame& source,uint32_t width,uint32_t height,std::span<uint8_t> output){
        initialize();
        // These buffers are shared with analysis/native decode. Revalidate their
        // actual size on every job; the cached tap key does not own the buffers.
        ensure(temp,width*source.height,16,true);
        ensure(out,width*height,4,true);ensure(outRead,width*height,4,false,true);
        if(iw!=source.width||ih!=source.height||ow!=width||oh!=height){
            // Invalidate before allocation: an interrupted resize cannot leave
            // a matching key attached to partially initialized resources.
            iw=ih=ow=oh=0;
            std::vector<Tap> taps;std::vector<Weight> weights;
            auto axis=[&](uint32_t input,uint32_t count){const double scale=double(input)/count,stretch=std::max(1.,scale);
                for(uint32_t i=0;i<count;++i){const double center=(i+.5)*scale-.5;const int a=int(std::ceil(center-stretch)),b=int(std::floor(center+stretch));double sum=0;
                    Tap t{uint32_t(weights.size()),uint32_t(b-a+1)};
                    for(int j=a;j<=b;++j){const double v=std::max(0.,1-std::abs((j-center)/stretch));sum+=v;weights.push_back({uint32_t(std::clamp(j,0,int(input)-1)),float(v)});}
                    for(uint32_t j=t.first;j<t.first+t.count;++j)weights[j].value=float(weights[j].value/sum);taps.push_back(t);
                }};
            axis(source.width,width);axis(source.height,height);
            ensure(tapBuffer,uint32_t(taps.size()),sizeof(Tap));ensure(weightBuffer,uint32_t(weights.size()),sizeof(Weight));
            upload(tapBuffer,taps.data(),tapBuffer.bytes);upload(weightBuffer,weights.data(),weightBuffer.bytes);
            iw=source.width;ih=source.height;ow=width;oh=height;
        }
        uploadSource(source);Parameters p{};p.plane[0][0]=source.width;p.plane[0][1]=source.height;p.plane[1][0]=width;p.plane[1][1]=height;parameters(p);
        ID3D11ShaderResourceView* s[]{nullptr,rgba.srv.Get(),tapBuffer.srv.Get(),weightBuffer.srv.Get(),nullptr};
        ID3D11UnorderedAccessView* u[]{nullptr,temp.uav.Get(),nullptr};
        context->CSSetShaderResources(0,5,s);context->CSSetUnorderedAccessViews(0,3,u,nullptr);context->CSSetShader(shaders[2].Get(),nullptr,0);
        context->Dispatch((width+15)/16,(source.height+15)/16,1);clearBindings();
        s[1]=nullptr;s[4]=temp.srv.Get();u[1]=nullptr;u[2]=out.uav.Get();
        context->CSSetShaderResources(0,5,s);context->CSSetUnorderedAccessViews(0,3,u,nullptr);context->CSSetShader(shaders[3].Get(),nullptr,0);
        context->Dispatch((width+15)/16,(height+15)/16,1);clearBindings();
        context->CopyResource(outRead.resource.Get(),out.resource.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        waitForGpu();checked(context->Map(outRead.resource.Get(),0,D3D11_MAP_READ,0,&mapped),"Read GPU resample");
        std::memcpy(output.data(),mapped.pData,output.size());context->Unmap(outRead.resource.Get(),0);
        activity.downloadedBytes+=output.size();++activity.resamples;
        retainResampled(output);
    }
};
Engine& engine(){static Engine instance;return instance;}
}
ScopedComputeBackend::ScopedComputeBackend(ComputeBackend backend):previous_(selectedBackend),previousStamp_(jobStamp){
    require(backend==ComputeBackend::Cpu||backend==ComputeBackend::Gpu,"Invalid compute backend.");selectedBackend=backend;jobStamp=backend==ComputeBackend::Gpu?++nextStamp:0;
}
ScopedComputeBackend::~ScopedComputeBackend(){selectedBackend=previous_;jobStamp=previousStamp_;}
ComputeBackend computeBackend(){return selectedBackend;}
GpuActivity gpuActivity(){return activity;}
GpuInfo gpuInfo(){auto& e=engine();std::lock_guard lock(e.mutex);try{e.initialize();}catch(const std::exception& error){e.fail(error);}return e.info;}
bool gpuWavelet(std::span<GpuPlane> planes,bool inverse,int first,int count){
    if(computeBackend()!=ComputeBackend::Gpu)return false;
    require(!planes.empty()&&planes.size()<=3&&first>=0&&count>0&&first+count<=6,"Invalid GPU transform.");
    for(const auto& p:planes)require(p.width>=64&&p.height>=64&&p.width%64==0&&p.height%64==0&&p.pixels.size()==size_t(p.width)*p.height&&p.width<=planes[0].width&&p.height<=planes[0].height,"Invalid GPU plane.");
    // Small transforms are faster in the established CPU vector path.
    if(uint64_t(planes[0].width>>first)*(planes[0].height>>first)<256*256)return false;
    auto& e=engine();std::lock_guard lock(e.mutex);try{e.transform(planes,inverse,first,count);return true;}catch(const std::exception& error){e.fail(error);return false;}
}
bool gpuResample(const Frame& source,uint32_t width,uint32_t height,std::span<uint8_t> output){
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(source.width)*source.height<512*512)return false;
    source.validate();require(output.size()==pixelBytes(width,height),"Invalid GPU resample output.");
    // Only the existing opaque downsampling route uses this kernel. Other
    // filters/alpha cases retain their production semantics on the CPU.
    auto& e=engine();std::lock_guard lock(e.mutex);try{e.resample(source,width,height,output);return true;}catch(const std::exception& error){e.fail(error);return false;}
}
bool gpuEdgeUpscale(const Frame& source,uint32_t width,uint32_t height,std::span<uint8_t> output,
                    std::span<const int> xLow,std::span<const float> xFraction,
                    std::span<const int> yLow,std::span<const float> yFraction,float sharpness){
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(width)*height<512*512)return false;
    source.validate();require(width>=source.width&&height>=source.height&&output.size()==pixelBytes(width,height)&&
        xLow.size()==width&&xFraction.size()==width&&yLow.size()==height&&yFraction.size()==height&&
        std::isfinite(sharpness)&&sharpness>=0&&sharpness<=1.5f,"Invalid GPU edge upscale.");
    auto& e=engine();std::lock_guard lock(e.mutex);
    try{e.edgeUpscale(source,width,height,output,xLow,xFraction,yLow,yFraction,sharpness);return true;}
    catch(const std::exception& error){e.fail(error);return false;}
}
bool gpuContourUpscale(const Frame& source,uint32_t width,uint32_t height,std::span<uint8_t> output,
                      std::span<const int> xLow,std::span<const float> xFraction,
                      std::span<const int> yLow,std::span<const float> yFraction,
                      float sharpness,float amount,float stronger,float curvature,std::span<const uint8_t> skipTiles,bool fused){
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(source.width)*source.height<512*512)return false;
    source.validate();
    require(width>=source.width&&height>=source.height&&output.size()==pixelBytes(width,height)&&
        xLow.size()==width&&xFraction.size()==width&&yLow.size()==height&&yFraction.size()==height&&
        std::isfinite(sharpness)&&sharpness>=0&&sharpness<=1.5f&&amount>0&&amount<=1&&
        stronger>=0&&stronger<=1&&curvature>=0&&curvature<=1,"Invalid GPU contour upscale.");
    const uint64_t tiles=uint64_t((source.width+95)/96)*((source.height+95)/96);
    require(skipTiles.empty()||skipTiles.size()==tiles,"Invalid GPU contour upscale mask.");
    if(tiles*12544*16>256ull*1024*1024||tiles>65535)return false;
    auto& e=engine();std::lock_guard lock(e.mutex);
    try{
        e.contourCleanup({},source.pixels,source.width,source.height,amount,stronger,curvature,skipTiles,fused,true);
        e.edgeUpscale(source,width,height,output,xLow,xFraction,yLow,yFraction,sharpness,&e.contourResult);return true;
    }catch(const std::exception& error){e.fail(error);return false;}
}
bool gpuSourceStatistics(const Frame& source,uint32_t cell,int mode,std::span<GpuSourceStatistics> output){
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(source.width)*source.height<512*512)return false;
    source.validate();require((cell==8||cell==16)&&output.size()==size_t((source.width+cell-1)/cell)*((source.height+cell-1)/cell),"Invalid GPU source analysis.");
    auto& e=engine();std::lock_guard lock(e.mutex);try{e.sourceStatistics(source,cell,mode,output);return true;}catch(const std::exception& error){e.fail(error);return false;}
}
bool gpuDecodeNative(std::span<GpuPlane> planes,const GpuNativeSettings& settings,std::span<uint8_t> output){
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(settings.width)*settings.height<512*512)return false;
    require(planes.size()==3&&output.size()==pixelBytes(settings.width,settings.height),"Invalid GPU native decode.");
    require(settings.cflMap.empty()||(settings.tile>=16&&settings.tile<=128&&settings.cflMap.size()==size_t((settings.width+settings.tile-1)/settings.tile)*((settings.height+settings.tile-1)/settings.tile)*2),"Invalid GPU CfL model.");
    auto& e=engine();std::lock_guard lock(e.mutex);try{e.decodeNative(planes,settings,output);return true;}catch(const std::exception& error){e.fail(error);return false;}
}

bool gpuSourceWavelet(const Frame& source,std::span<GpuPlane> planes,uint32_t tile,std::span<const uint8_t> map,std::span<const float> means){
    if(computeBackend()!=ComputeBackend::Gpu||planes.size()!=3||!jobStamp)return false;
    require(tile==64&&map.size()==means.size()*2&&means.size()==size_t((source.width+tile-1)/tile)*((source.height+tile-1)/tile),"Invalid GPU source wavelet model.");
    for(const auto& p:planes)require(p.width>=64&&p.height>=64&&p.width%64==0&&p.height%64==0&&p.pixels.size()==size_t(p.width)*p.height,"Invalid GPU source wavelet planes.");
    auto& e=engine();
    {std::lock_guard lock(e.mutex);
        try{if(!e.sourceWavelet(source,planes,tile,map,means))return false;}
        catch(const std::exception& error){e.fail(error);return false;}}
    for(auto& p:planes)waveletTransform(p.pixels,int(p.width),int(p.height),1,false,true,true,3,3);
    return true;
}
bool gpuSourceColor(const Frame& source,std::span<GpuPlane> planes){
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(source.width)*source.height<512*512)return false;
    source.validate();require(planes.size()==3,"Invalid GPU color planes.");
    auto& e=engine();std::lock_guard lock(e.mutex);try{e.sourceColor(source,planes);return true;}catch(const std::exception& error){e.fail(error);return false;}
}
bool gpuSourceColorModel(const Frame& source,std::span<GpuPlane> planes,uint32_t tile,std::span<GpuColorStatistics> fitted,std::span<ColorModelPatch> patches){
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(source.width)*source.height<512*512)return false;
    source.validate();require(planes.size()==3&&(tile==32||tile==64)&&
        fitted.size()==size_t((source.width+tile-1)/tile)*((source.height+tile-1)/tile)&&
        (patches.empty()||(tile==64&&patches.size()==size_t((source.width+tile-1)/tile+1)*((source.height+tile-1)/tile+1))),"Invalid GPU color fit.");
    auto& e=engine();std::lock_guard lock(e.mutex);
    try{e.initialize();if(!e.doubles)return false;e.sourceColor(source,planes,tile,fitted,patches);
        e.retainSourceColor(source,planes);return true;}
    catch(const std::exception& error){e.fail(error);return false;}
}

bool gpuContourCleanup(std::span<uint8_t> output,std::span<const uint8_t> source,uint32_t width,uint32_t height,
                       float amount,float stronger,float curvature,std::span<const uint8_t> skipTiles,bool fused){
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(width)*height<512*512)return false;
    require(output.size()==pixelBytes(width,height)&&source.size()==output.size()&&amount>0&&amount<=1&&stronger>=0&&stronger<=1&&curvature>=0&&curvature<=1,"Invalid GPU contour cleanup.");
    const uint64_t tiles=uint64_t((width+95)/96)*((height+95)/96);
    require(skipTiles.empty()||skipTiles.size()==tiles,"Invalid GPU contour mask.");
    // Large frames keep the CPU path rather than attempting an oversized GPU
    // allocation or counting normal capacity selection as a device failure.
    if(tiles*12544*16>256ull*1024*1024||tiles>65535)return false;
    auto& e=engine();std::lock_guard lock(e.mutex);try{e.contourCleanup(output,source,width,height,amount,stronger,curvature,skipTiles,fused);return true;}catch(const std::exception& error){e.fail(error);return false;}
}
bool gpuFinalSharpen(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t strength){
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(width)*height<512*512)return false;
    require(pixels.size()==pixelBytes(width,height)&&strength<=100,"Invalid GPU final sharpening.");
    if(!strength)return true;
    auto& e=engine();std::lock_guard lock(e.mutex);
    try{e.finalSharpen(pixels,width,height,strength);return true;}catch(const std::exception& error){e.fail(error);return false;}
}

}
