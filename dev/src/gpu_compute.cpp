#include "gpu_compute.hpp"
#include "gpu_kernels.hpp"
#include "perceptual_color.hpp"
#include "wavelet.hpp"
#include "artifact_cleanup.hpp"
#include "sparse_coefficients_avx2.hpp"
#include "timing.hpp"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <mutex>
#include <exception>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <atomic>
#include <bit>
#include <cmath>
namespace fajk {
bool hasAvx2();
bool nativeReciprocalTableAvx2(std::array<uint32_t,4096>&);
namespace {
using Microsoft::WRL::ComPtr;
thread_local ComputeBackend selectedBackend=ComputeBackend::Cpu;
thread_local uint64_t jobStamp=0;
thread_local GpuDecodedSource* decodedSourceCapture=nullptr;
std::atomic<uint64_t> nextStamp{0};
thread_local GpuActivity activity;
void checked(HRESULT hr,const char* operation){
    if(FAILED(hr)){std::ostringstream s;s<<operation<<" (0x"<<std::hex<<uint32_t(hr)<<")";throw std::runtime_error(s.str());}
}
struct Buffer {
    ComPtr<ID3D11Buffer> resource;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    uint32_t bytes=0,capacity=0,stride=0;
};
struct ReadMapping {
    ID3D11DeviceContext* context;ID3D11Buffer* buffer;D3D11_MAPPED_SUBRESOURCE mapped{};
    ReadMapping(ID3D11DeviceContext* c,Buffer& b):context(c),buffer(b.resource.Get()){
        checked(context->Map(buffer,0,D3D11_MAP_READ,0,&mapped),"Read GPU predictive preparation");
    }
    ReadMapping(const ReadMapping&)=delete;
    ReadMapping& operator=(const ReadMapping&)=delete;
    ~ReadMapping(){context->Unmap(buffer,0);}
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
    Buffer sparseUpload;
    std::vector<SparseCoefficient> sparseCoefficients;
    Buffer motionReference,motionCoarse,motionFine,motionRead,motionAffine,predictiveWitness;
    Buffer predictiveWarp,predictiveWarpWitness,predictiveSelected,predictiveSelectedWitness,predictiveModes,predictiveRead;
    std::array<ColorSourceSlot,2> colorSources;
    SourceSlot currentSource;
    std::array<SourceSlot,3> sourceCache;
    Buffer& rgba=currentSource.buffer;
    uint64_t sourceClock=0;
    Buffer temp,out,outRead,tapBuffer,weightBuffer,upscaleAxes;
    // Native reconstruction and final display have different stable sizes.
    // Keeping their surfaces separate avoids reallocating both on every frame.
    Buffer displayOut,displayRead,displayFinal;
    Buffer artifactGuides,artifactRowsBuffer,artifactResult,artifactReciprocal;
    std::vector<float> artifactPacked;
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
            if(((i>=19&&i<=21)||(i>=31&&i<=32))&&!doubles)continue;
            checked(device->CreateComputeShader(gpuKernels[i].bytes,gpuKernels[i].size,nullptr,&shaders[i]),"Create R470 GPU kernel");
        }
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Parameters);cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        checked(device->CreateBuffer(&cb,nullptr,&constants),"Create GPU parameters");info.available=true;info.error.clear();
    }
    void ensure(Buffer& b,uint32_t count,uint32_t stride,bool writable=false,bool readback=false){
        const uint64_t bytes=uint64_t(count)*stride;
        if(!bytes||bytes>256ull*1024*1024)throw std::runtime_error("GPU working buffer exceeds 256 MiB safety limit.");
        // Native inverse and full-source analysis alternate within one frame.
        // Retain moderate buffers across those stages; trim unusually large
        // capacities once their requested logical size drops by over fourfold.
        if(b.resource&&b.capacity>=bytes&&b.capacity<=std::max<uint64_t>(64ull*1024*1024,bytes*4)&&b.stride==stride&&
           (readback?!b.srv:bool(b.srv))&&(!writable||b.uav)){
            if(&b==&rgba&&b.bytes!=bytes){currentSource.stamp=0;currentSource.pointer=nullptr;}
            b.bytes=uint32_t(bytes);return;
        }
        if(&b==&rgba){currentSource.stamp=0;currentSource.pointer=nullptr;}
        // Keep bounded capacity across nearby working geometries. Logical byte
        // lengths remain exact for uploads, shader parameters and readbacks.
        const auto quantum=std::max(1u,4096/stride);
        const uint64_t capacityCount=std::min<uint64_t>(256ull*1024*1024/stride,
            ((uint64_t(count)+count/4+quantum-1)/quantum)*quantum);
        const auto capacity=uint32_t(capacityCount*stride);
        b={};D3D11_BUFFER_DESC d{};d.ByteWidth=capacity;
        d.Usage=readback?D3D11_USAGE_STAGING:D3D11_USAGE_DEFAULT;
        d.BindFlags=readback?0:D3D11_BIND_SHADER_RESOURCE|(writable?D3D11_BIND_UNORDERED_ACCESS:0);
        d.CPUAccessFlags=readback?D3D11_CPU_ACCESS_READ:0;
        d.MiscFlags=readback?0:D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=readback?0:stride;
        checked(device->CreateBuffer(&d,nullptr,&b.resource),"Allocate GPU buffer");
        ++activity.bufferAllocations;activity.allocatedBufferBytes+=capacity;
        if(!readback){checked(device->CreateShaderResourceView(b.resource.Get(),nullptr,&b.srv),"Create GPU input");
            if(writable)checked(device->CreateUnorderedAccessView(b.resource.Get(),nullptr,&b.uav),"Create GPU output");}
        b.bytes=uint32_t(bytes);b.capacity=capacity;b.stride=stride;
    }
    void parameters(const Parameters& p){context->UpdateSubresource(constants.Get(),0,nullptr,&p,0,0);auto* cb=constants.Get();context->CSSetConstantBuffers(0,1,&cb);}
    void clearBindings(){if(!context)return;ID3D11ShaderResourceView* s[11]{};ID3D11UnorderedAccessView* u[5]{};
        context->CSSetShaderResources(0,11,s);context->CSSetUnorderedAccessViews(0,5,u,nullptr);}
    void upload(Buffer& b,const void* data,uint32_t bytes,uint32_t offset=0){
        require(offset<=b.bytes&&bytes<=b.bytes-offset,"GPU upload exceeds the logical buffer.");
        D3D11_BOX box{offset,0,0,offset+bytes,1,1};context->UpdateSubresource(b.resource.Get(),0,&box,data,0,0);activity.uploadedBytes+=bytes;
    }
    void copy(Buffer& destination,const Buffer& source,uint32_t offset=0){
        require(source.resource&&destination.resource&&source.bytes&&offset<=destination.bytes&&source.bytes<=destination.bytes-offset,"GPU copy exceeds the logical buffer.");
        D3D11_BOX box{0,0,0,source.bytes,1,1};
        context->CopySubresourceRegion(destination.resource.Get(),0,offset,0,0,source.resource.Get(),0,&box);
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
            if(currentSource.stamp||rgba.capacity<bytes||rgba.capacity>std::max<uint64_t>(65536,uint64_t(bytes)*4)){
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
    void retainImage(std::span<const uint8_t> pixels,uint64_t stamp){
        // The host still needs these bytes, but the following GPU color and
        // source analysis can consume the existing packed result directly.
        // A source job stamp, not a pointer alone, defines immutability.
        if(!stamp||pixels.size()>32u*1024*1024)return;
        // A producer can replace bytes at the same address within one job.
        // There must never be two cache entries claiming that same identity.
        auto invalidate=[&](SourceSlot& slot){if(slot.pointer==pixels.data()&&slot.stamp==stamp){slot.pointer=nullptr;slot.stamp=0;}};
        invalidate(currentSource);for(auto& slot:sourceCache)invalidate(slot);
        auto* chosen=&sourceCache[0];
        for(auto& slot:sourceCache)if(slot.touched<chosen->touched)chosen=&slot;
        std::swap(chosen->buffer,out);
        chosen->pointer=pixels.data();chosen->stamp=stamp;chosen->touched=++sourceClock;
    }
    void retainResampled(std::span<const uint8_t> pixels){retainImage(pixels,jobStamp);}
    void waitForGpu(const std::function<void()>& whilePending={}){
        // A blocking Map can spin inside the driver. Sleep on the GPU completion
        // event instead, so offloading also releases CPU cores to other work.
        if(ready&&dxgiDevice){context->Flush();checked(dxgiDevice->EnqueueSetEvent(ready),"Schedule GPU completion event");
            if(whilePending)whilePending();
            if(WaitForSingleObject(ready,10000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU completion timeout.");}
        else if(whilePending){context->Flush();whilePending();}
    }
    void fail(const std::exception& e){clearBindings();++activity.fallbacks;info.error=e.what();
        // Geometry/resource-limit errors may recover on the next image. A removed
        // device or failed initialization remains unavailable, without retry loops.
        if(!device||FAILED(device->GetDeviceRemovedReason())||!shaders.front())info.available=false;
    }
    void transform(std::span<GpuPlane> planes,bool inverse,int first,int levels,bool download=true,uint32_t cleanup=0,bool uploaded=false,bool canonical=false){
        initialize();Parameters p{};p.options[0]=cleanup;p.count=uint32_t(planes.size());p.inverse=inverse;uint32_t total=0;
        if(canonical)p.unused=hasAvx2()?8:4;
        for(size_t c=0;c<planes.size();++c){const auto& plane=planes[c];p.plane[c][0]=plane.width;p.plane[c][1]=plane.height;p.plane[c][2]=total;total+=uint32_t(plane.pixels.size());}
        ensure(waveA,total,4,true);ensure(waveB,total,4,true);if(download)ensure(waveRead,total,4,false,true);
        if(!uploaded)for(size_t c=0;c<planes.size();++c)upload(waveA,planes[c].pixels.data(),uint32_t(planes[c].pixels.size_bytes()),p.plane[c][2]*4);
        auto dispatch=[&](bool vertical,Buffer& input,Buffer& output){
            auto* srv=input.srv.Get();auto* uav=output.uav.Get();context->CSSetShaderResources(0,1,&srv);context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
            context->CSSetShader(shaders[(canonical?31:inverse?12:0)+(vertical?1:0)].Get(),nullptr,0);
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
        copy(waveRead,waveA);
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
            copy(colorStatsRead,colorStats);
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
                copy(colorPatchesRead,colorPatches);
            }
        }
        copy(waveRead,waveA);D3D11_MAPPED_SUBRESOURCE mapped{};
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
                     std::span<const int> yLow,std::span<const float> yFraction,float sharpness,Buffer* resident=nullptr,uint32_t sharpening=0){
        TimingTimeline timing;initialize();
        struct Axis {int low;float fraction;};
        std::vector<Axis> axes(size_t(width)+height);
        for(uint32_t i=0;i<width;++i)axes[i]={xLow[i],xFraction[i]};
        for(uint32_t i=0;i<height;++i)axes[width+i]={yLow[i],yFraction[i]};
        ensure(upscaleAxes,uint32_t(axes.size()),sizeof(Axis));
        ensure(displayOut,width*height,4,true);ensure(displayRead,width*height,4,false,true);
        // CPU cleanup/text restoration has changed these pixels. Never reuse
        // an immutable encoder source entry based on the recycled address.
        if(!resident){
            activateSource(uint32_t(source.pixels.size()),source.pixels.data(),0);
            upload(rgba,source.pixels.data(),uint32_t(source.pixels.size()));
        }
        upload(upscaleAxes,axes.data(),upscaleAxes.bytes);
        Parameters p{};p.plane[0][0]=source.width;p.plane[0][1]=source.height;
        p.image[0]=width;p.image[1]=height;p.restoration[0]=std::bit_cast<uint32_t>(sharpness);parameters(p);
        auto* raw=(resident?resident:&rgba)->srv.Get();auto* axis=upscaleAxes.srv.Get();auto* result=displayOut.uav.Get();
        context->CSSetShaderResources(1,1,&raw);context->CSSetShaderResources(2,1,&axis);
        context->CSSetUnorderedAccessViews(2,1,&result,nullptr);context->CSSetShader(shaders[18].Get(),nullptr,0);
        context->Dispatch((width+31)/32,(height+7)/8,1);clearBindings();
        timing.mark("decode.gpu.upscale.prepare_submit");
        Buffer* final=&displayOut;
        if(sharpening){
            ensure(displayFinal,width*height,4,true);
            Parameters sharp{};sharp.image[0]=width;sharp.image[1]=height;sharp.image[2]=sharpening;parameters(sharp);
            auto* input=displayOut.srv.Get();auto* target=displayFinal.uav.Get();
            context->CSSetShaderResources(1,1,&input);context->CSSetUnorderedAccessViews(2,1,&target,nullptr);
            context->CSSetShader(shaders[23].Get(),nullptr,0);context->Dispatch((width+15)/16,(height+15)/16,1);clearBindings();
            final=&displayFinal;++activity.sharpens;
        }
        copy(displayRead,*final);D3D11_MAPPED_SUBRESOURCE mapped{};
        waitForGpu();timing.mark("decode.gpu.upscale.wait");checked(context->Map(displayRead.resource.Get(),0,D3D11_MAP_READ,0,&mapped),"Read GPU edge upscale");
        std::memcpy(target.data(),mapped.pData,target.size());context->Unmap(displayRead.resource.Get(),0);
        timing.mark("decode.gpu.upscale.readback_copy");
        activity.downloadedBytes+=target.size();++activity.resamples;
    }
    void blockMotion(const Frame& current,const Frame& reference,std::span<uint32_t> vectors,std::span<uint32_t> affine){
        initialize();uploadSource(current);
        ensure(motionReference,reference.width*reference.height,4);upload(motionReference,reference.pixels.data(),uint32_t(reference.pixels.size()));
        const uint32_t cw=(current.width+127)/128,ch=(current.height+127)/128,fw=(current.width+31)/32,fh=(current.height+31)/32;
        ensure(motionCoarse,cw*ch,4,true);ensure(motionFine,fw*fh,4,true);ensure(motionRead,fw*fh*(affine.empty()?1:2),4,false,true);
        Parameters p{};p.image[0]=current.width;p.image[1]=current.height;p.plane[0][0]=cw;p.plane[0][1]=ch;
        for(unsigned pass=0;pass<2;++pass){
            p.image[2]=pass?32:128;p.image[3]=pass;parameters(p);
            auto* a=rgba.srv.Get();auto* b=motionReference.srv.Get();auto* seeds=pass?motionCoarse.srv.Get():nullptr;auto* result=pass?motionFine.uav.Get():motionCoarse.uav.Get();
            context->CSSetShaderResources(1,1,&a);context->CSSetShaderResources(6,1,&b);context->CSSetShaderResources(8,1,&seeds);context->CSSetUnorderedAccessViews(2,1,&result,nullptr);
            context->CSSetShader(shaders[25].Get(),nullptr,0);context->Dispatch(pass?fw:cw,pass?fh:ch,1);clearBindings();
        }
        if(!affine.empty()){
            ensure(motionAffine,fw*fh,4,true);p.plane[0][0]=fw;p.plane[0][1]=fh;parameters(p);
            auto* a=rgba.srv.Get();auto* b=motionReference.srv.Get();auto* field=motionFine.srv.Get();auto* result=motionAffine.uav.Get();
            context->CSSetShaderResources(1,1,&a);context->CSSetShaderResources(6,1,&b);context->CSSetShaderResources(8,1,&field);context->CSSetUnorderedAccessViews(2,1,&result,nullptr);
            context->CSSetShader(shaders[26].Get(),nullptr,0);context->Dispatch(fw,fh,1);clearBindings();
        }
        copy(motionRead,motionFine);
        if(!affine.empty())copy(motionRead,motionAffine,fw*fh*4);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        waitForGpu();checked(context->Map(motionRead.resource.Get(),0,D3D11_MAP_READ,0,&mapped),"Read GPU block motion");
        std::memcpy(vectors.data(),mapped.pData,vectors.size_bytes());if(!affine.empty())std::memcpy(affine.data(),static_cast<const uint8_t*>(mapped.pData)+vectors.size_bytes(),affine.size_bytes());
        context->Unmap(motionRead.resource.Get(),0);activity.downloadedBytes+=vectors.size_bytes()+affine.size_bytes();
    }
    void dispatchMotionWarp(const Frame& source,uint32_t cell,std::span<const uint32_t> vectors,const std::array<int,3>& bias,std::span<const uint8_t> modes,bool independentBlocks,std::span<const uint32_t> affine){
        initialize();activateSource(uint32_t(source.pixels.size()),source.pixels.data(),0);upload(rgba,source.pixels.data(),uint32_t(source.pixels.size()));
        ensure(slopes,uint32_t(vectors.size()),4);upload(slopes,vectors.data(),uint32_t(vectors.size_bytes()));
        if(!modes.empty()){std::vector<uint32_t> mask(modes.begin(),modes.end());ensure(guides,uint32_t(mask.size()),4);upload(guides,mask.data(),guides.bytes);}
        if(!affine.empty()){ensure(means,uint32_t(affine.size()),4);upload(means,affine.data(),means.bytes);}
        ensure(out,source.width*source.height,4,true);ensure(outRead,source.width*source.height,4,false,true);
        Parameters p{};p.image[0]=source.width;p.image[1]=source.height;p.image[2]=cell;p.image[3]=!modes.empty();p.plane[0][0]=(source.width+cell-1)/cell;p.plane[0][1]=(source.height+cell-1)/cell;p.plane[0][2]=(source.width+7)/8;
        p.options[0]=independentBlocks;p.options[1]=!affine.empty();for(int c=0;c<3;++c)p.restoration[c]=std::bit_cast<uint32_t>(int32_t(bias[c]));parameters(p);
        auto* raw=rgba.srv.Get();auto* motion=slopes.srv.Get();auto* mask=modes.empty()?nullptr:guides.srv.Get();auto* result=out.uav.Get();
        context->CSSetShaderResources(1,1,&raw);context->CSSetShaderResources(6,1,&motion);context->CSSetShaderResources(8,1,&mask);context->CSSetUnorderedAccessViews(2,1,&result,nullptr);
        if(!affine.empty()){auto* gradient=means.srv.Get();context->CSSetShaderResources(7,1,&gradient);}
        context->CSSetShader(shaders[24].Get(),nullptr,0);context->Dispatch((source.width+15)/16,(source.height+15)/16,1);clearBindings();
    }
    void motionWarp(const Frame& source,uint32_t cell,std::span<const uint32_t> vectors,const std::array<int,3>& bias,std::span<const uint8_t> modes,std::span<uint8_t> target,bool independentBlocks,std::span<const uint32_t> affine,const std::function<void()>& whilePending){
        dispatchMotionWarp(source,cell,vectors,bias,modes,independentBlocks,affine);
        copy(outRead,out);D3D11_MAPPED_SUBRESOURCE mapped{};
        waitForGpu(whilePending);checked(context->Map(outRead.resource.Get(),0,D3D11_MAP_READ,0,&mapped),"Read GPU motion prediction");
        std::memcpy(target.data(),mapped.pData,target.size());context->Unmap(outRead.resource.Get(),0);activity.downloadedBytes+=target.size();
    }
    bool uploadSparseCoefficients(std::span<GpuPlane> planes,uint32_t total){
        // Reject dense data using bounded, irregular probes before allocating or
        // scanning it. A mistaken rejection affects speed only, never pixels.
        uint32_t state=0x9e3779b9;unsigned nonzero=0;
        for(const auto& p:planes)for(unsigned probe=0;probe<48;++probe){state=state*1664525+1013904223;nonzero+=std::bit_cast<uint32_t>(p.pixels[state%p.pixels.size()])!=0;}
        if(nonzero>48)return false;
        const size_t limit=std::min<size_t>(total/4,1u<<20);sparseCoefficients.clear();uint32_t offset=0;
        if(sparseCoefficients.capacity()<4096)sparseCoefficients.reserve(std::min<size_t>(limit,4096));
        for(const auto& p:planes){
            if(hasAvx2()){if(!appendSparseCoefficientsAvx2(p.pixels,offset,limit,sparseCoefficients))return false;}
            else for(size_t i=0;i<p.pixels.size();++i)if(const auto bits=std::bit_cast<uint32_t>(p.pixels[i]);bits){
                if(sparseCoefficients.size()==limit)return false;sparseCoefficients.push_back({offset+uint32_t(i),bits});}
            offset+=uint32_t(p.pixels.size());
        }
        ensure(waveA,total,4,true);const UINT zero[4]{};context->ClearUnorderedAccessViewUint(waveA.uav.Get(),zero);
        if(!sparseCoefficients.empty()){
            const uint32_t count=uint32_t(sparseCoefficients.size());if(sparseUpload.bytes<count*8)ensure(sparseUpload,std::bit_ceil(count),8);upload(sparseUpload,sparseCoefficients.data(),count*8);
            Parameters p{};p.count=count;parameters(p);auto* input=sparseUpload.srv.Get();auto* result=waveA.uav.Get();
            context->CSSetShaderResources(9,1,&input);context->CSSetUnorderedAccessViews(0,1,&result,nullptr);context->CSSetShader(shaders[33].Get(),nullptr,0);
            context->Dispatch((count+255)/256,1,1);clearBindings();
        }
        ++activity.sparseCoefficientUploads;return true;
    }
    void reconstructMotion(const Frame& source,std::span<const uint32_t> vectors,const std::array<int,3>& bias,std::span<const uint8_t> modes,std::span<const uint32_t> affine,std::span<GpuPlane> planes,uint32_t filterStep,std::span<uint8_t> target,const std::function<void()>& whilePending,bool coefficients,bool adjacentFilter){
        dispatchMotionWarp(source,32,vectors,bias,modes,true,affine);
        // Submit before independent CPU synthesis. Keep prediction, modes and
        // gradients resident; no intermediate fence or packed readback.
        if(!coefficients){context->Flush();if(whilePending)whilePending();}
        Parameters p{};p.image[0]=source.width;p.image[1]=source.height;p.image[2]=filterStep;p.image[3]=adjacentFilter;
        uint32_t total=0;for(unsigned c=0;c<3;++c){p.plane[c][0]=planes[c].width;p.plane[c][1]=planes[c].height;p.plane[c][2]=total;total+=uint32_t(planes[c].pixels.size());}
        if(coefficients){context->Flush();const bool uploaded=uploadSparseCoefficients(planes,total);transform(planes,true,0,6,false,0,uploaded,true);}
        else{ensure(waveA,total,4,true);for(unsigned c=0;c<3;++c)upload(waveA,planes[c].pixels.data(),uint32_t(planes[c].pixels.size_bytes()),p.plane[c][2]*4);}
        ensure(predictiveSelected,source.width*source.height,4,true);parameters(p);
        auto* samples=waveA.srv.Get();auto* prediction=out.srv.Get();auto* mask=guides.srv.Get();auto* corrected=predictiveSelected.uav.Get();
        context->CSSetShaderResources(0,1,&samples);context->CSSetShaderResources(1,1,&prediction);context->CSSetShaderResources(8,1,&mask);context->CSSetUnorderedAccessViews(2,1,&corrected,nullptr);
        context->CSSetShader(shaders[30].Get(),nullptr,0);context->Dispatch((source.width+15)/16,(source.height+15)/16,1);clearBindings();
        Buffer* final=&predictiveSelected;
        if(filterStep>=4){
            auto* input=predictiveSelected.srv.Get();auto* filtered=out.uav.Get();
            context->CSSetShaderResources(1,1,&input);context->CSSetShaderResources(6,1,&mask);context->CSSetUnorderedAccessViews(2,1,&filtered,nullptr);
            context->CSSetShader(shaders[29].Get(),nullptr,0);context->Dispatch((source.width+15)/16,(source.height+15)/16,1);clearBindings();final=&out;
        }
        copy(outRead,*final);waitForGpu();
        ReadMapping mapped(context.Get(),outRead);std::memcpy(target.data(),mapped.mapped.pData,target.size());
        activity.downloadedBytes+=target.size();++activity.motionReconstructions;if(filterStep>=4)++activity.referenceFilters;
    }
    void predictiveWavelet(const Frame& current,const Frame& predicted,const Frame& witness,std::span<const uint8_t> keep,uint32_t propagatedError,std::span<GpuPlane> planes,bool lumaErrorOnly){
        initialize();uploadSource(current);
        ensure(motionReference,current.width*current.height,4);upload(motionReference,predicted.pixels.data(),uint32_t(predicted.pixels.size()));
        ensure(predictiveWitness,current.width*current.height,4);upload(predictiveWitness,witness.pixels.data(),uint32_t(witness.pixels.size()));
        const std::vector<uint32_t> modes(keep.begin(),keep.end());ensure(guides,uint32_t(modes.size()),4);upload(guides,modes.data(),uint32_t(modes.size()*4));
        Parameters p{};p.count=3;p.image[0]=current.width;p.image[1]=current.height;p.image[2]=propagatedError;p.image[3]=lumaErrorOnly;uint32_t total=0;
        for(size_t c=0;c<3;++c){p.plane[c][0]=planes[c].width;p.plane[c][1]=planes[c].height;p.plane[c][2]=total;total+=uint32_t(planes[c].pixels.size());}
        ensure(waveA,total,4,true);parameters(p);
        auto* a=rgba.srv.Get();auto* b=motionReference.srv.Get();auto* c=predictiveWitness.srv.Get();auto* mask=guides.srv.Get();auto* result=waveA.uav.Get();
        context->CSSetShaderResources(1,1,&a);context->CSSetShaderResources(6,1,&b);context->CSSetShaderResources(7,1,&c);context->CSSetShaderResources(8,1,&mask);context->CSSetUnorderedAccessViews(0,1,&result,nullptr);
        context->CSSetShader(shaders[27].Get(),nullptr,0);context->Dispatch((planes[1].width+15)/16,(planes[1].height+15)/16,1);clearBindings();
        // The float residual has never left waveA. Only final coefficients are
        // downloaded by transform; there is one completion fence for both.
        transform(planes,false,0,6,true,0,true);
    }
    void preparePredictive(const Frame& current,const Frame& sourceReference,const Frame& decodedReference,
                           std::span<const uint32_t> vectors,std::span<const uint32_t> affine,const std::array<int,3>& bias,
                           uint32_t tolerance,uint32_t propagatedError,std::span<uint8_t> predicted,std::span<uint8_t> keep,std::span<GpuPlane> planes,bool lumaErrorOnly){
        initialize();uploadSource(current);
        const uint32_t pixels=current.width*current.height,blocks=uint32_t(keep.size());
        ensure(motionReference,pixels,4);upload(motionReference,decodedReference.pixels.data(),pixels*4);
        ensure(predictiveWitness,pixels,4);upload(predictiveWitness,sourceReference.pixels.data(),pixels*4);
        ensure(slopes,uint32_t(vectors.size()),4);upload(slopes,vectors.data(),uint32_t(vectors.size_bytes()));
        ensure(means,uint32_t(affine.size()),4);upload(means,affine.data(),uint32_t(affine.size_bytes()));
        ensure(predictiveWarp,pixels,4,true);ensure(predictiveWarpWitness,pixels,4,true);
        ensure(predictiveSelected,pixels,4,true);ensure(predictiveSelectedWitness,pixels,4,true);ensure(predictiveModes,blocks,4,true);
        Parameters p{};p.image[0]=current.width;p.image[1]=current.height;p.image[2]=32;
        p.plane[0][0]=(current.width+31)/32;p.plane[0][1]=(current.height+31)/32;p.options[0]=p.options[1]=1;
        for(int c=0;c<3;++c)p.restoration[c]=std::bit_cast<uint32_t>(int32_t(bias[c]));parameters(p);
        auto warp=[&](Buffer& source,Buffer& target){
            auto* raw=source.srv.Get();auto* motion=slopes.srv.Get();auto* gradient=means.srv.Get();auto* result=target.uav.Get();
            context->CSSetShaderResources(1,1,&raw);context->CSSetShaderResources(6,1,&motion);context->CSSetShaderResources(7,1,&gradient);
            context->CSSetUnorderedAccessViews(2,1,&result,nullptr);context->CSSetShader(shaders[24].Get(),nullptr,0);
            context->Dispatch((current.width+15)/16,(current.height+15)/16,1);clearBindings();
        };
        warp(motionReference,predictiveWarp);warp(predictiveWitness,predictiveWarpWitness);
        p.options[0]=tolerance;parameters(p);
        auto* actual=rgba.srv.Get();auto* decoded=motionReference.srv.Get();auto* source=predictiveWitness.srv.Get();
        auto* moving=predictiveWarp.srv.Get();auto* witness=predictiveWarpWitness.srv.Get();
        auto* selected=predictiveSelected.uav.Get();auto* selectedSource=predictiveSelectedWitness.uav.Get();auto* modes=predictiveModes.uav.Get();
        context->CSSetShaderResources(1,1,&actual);context->CSSetShaderResources(6,1,&decoded);context->CSSetShaderResources(7,1,&source);
        context->CSSetShaderResources(9,1,&moving);context->CSSetShaderResources(10,1,&witness);
        context->CSSetUnorderedAccessViews(2,1,&selected,nullptr);context->CSSetUnorderedAccessViews(3,1,&selectedSource,nullptr);context->CSSetUnorderedAccessViews(4,1,&modes,nullptr);
        context->CSSetShader(shaders[28].Get(),nullptr,0);context->Dispatch((current.width+7)/8,(current.height+7)/8,1);clearBindings();
        p.count=3;p.image[2]=propagatedError;p.image[3]=lumaErrorOnly;uint32_t total=0;
        for(size_t c=0;c<3;++c){p.plane[c][0]=planes[c].width;p.plane[c][1]=planes[c].height;p.plane[c][2]=total;total+=uint32_t(planes[c].pixels.size());}
        ensure(waveA,total,4,true);ensure(waveRead,total,4,false,true);parameters(p);
        auto* prediction=predictiveSelected.srv.Get();auto* selectedWitness=predictiveSelectedWitness.srv.Get();auto* mask=predictiveModes.srv.Get();auto* result=waveA.uav.Get();
        context->CSSetShaderResources(1,1,&actual);context->CSSetShaderResources(6,1,&prediction);context->CSSetShaderResources(7,1,&selectedWitness);context->CSSetShaderResources(8,1,&mask);
        context->CSSetUnorderedAccessViews(0,1,&result,nullptr);context->CSSetShader(shaders[27].Get(),nullptr,0);
        context->Dispatch((planes[1].width+15)/16,(planes[1].height+15)/16,1);clearBindings();
        transform(planes,false,0,6,false,0,true);
        ensure(predictiveRead,pixels+blocks,4,false,true);
        copy(waveRead,waveA);
        copy(predictiveRead,predictiveSelected);
        copy(predictiveRead,predictiveModes,pixels*4);
        waitForGpu();ReadMapping coefficients(context.Get(),waveRead),predictionAndModes(context.Get(),predictiveRead);
        // All fallible operations precede host writes, preserving CPU fallback.
        for(size_t c=0;c<3;++c)std::memcpy(planes[c].pixels.data(),static_cast<const float*>(coefficients.mapped.pData)+p.plane[c][2],planes[c].pixels.size_bytes());
        std::memcpy(predicted.data(),predictionAndModes.mapped.pData,predicted.size());
        const auto* values=static_cast<const uint32_t*>(predictionAndModes.mapped.pData)+pixels;
        for(size_t i=0;i<keep.size();++i)keep[i]=uint8_t(values[i]);
        activity.downloadedBytes+=uint64_t(total+pixels+blocks)*4;++activity.analyses;
    }
    void finalSharpen(std::span<uint8_t> target,uint32_t width,uint32_t height,uint32_t strength){
        initialize();activateSource(uint32_t(target.size()),target.data(),0);upload(rgba,target.data(),uint32_t(target.size()));
        ensure(out,width*height,4,true);ensure(outRead,width*height,4,false,true);
        Parameters p{};p.image[0]=width;p.image[1]=height;p.image[2]=strength;parameters(p);
        auto* raw=rgba.srv.Get();auto* result=out.uav.Get();context->CSSetShaderResources(1,1,&raw);context->CSSetUnorderedAccessViews(2,1,&result,nullptr);
        context->CSSetShader(shaders[23].Get(),nullptr,0);context->Dispatch((width+15)/16,(height+15)/16,1);clearBindings();
        copy(outRead,out);D3D11_MAPPED_SUBRESOURCE mapped{};
        waitForGpu();checked(context->Map(outRead.resource.Get(),0,D3D11_MAP_READ,0,&mapped),"Read GPU final sharpening");
        std::memcpy(target.data(),mapped.pData,target.size());context->Unmap(outRead.resource.Get(),0);
        activity.downloadedBytes+=target.size();++activity.sharpens;
    }
    void referenceFilter(Frame& frame,uint32_t step,std::span<const uint8_t> keep,bool adjacent){
        initialize();activateSource(uint32_t(frame.pixels.size()),frame.pixels.data(),0);
        upload(rgba,frame.pixels.data(),uint32_t(frame.pixels.size()));
        const auto pixels=frame.width*frame.height;ensure(out,pixels,4,true);ensure(outRead,pixels,4,false,true);
        std::vector<uint32_t> modes(keep.begin(),keep.end());ensure(slopes,uint32_t(modes.size()),4);upload(slopes,modes.data(),slopes.bytes);
        Parameters p{};p.image[0]=frame.width;p.image[1]=frame.height;p.image[2]=step;p.image[3]=adjacent;parameters(p);
        auto* raw=rgba.srv.Get();auto* mask=slopes.srv.Get();auto* result=out.uav.Get();
        context->CSSetShaderResources(1,1,&raw);context->CSSetShaderResources(6,1,&mask);context->CSSetUnorderedAccessViews(2,1,&result,nullptr);
        context->CSSetShader(shaders[29].Get(),nullptr,0);context->Dispatch((frame.width+15)/16,(frame.height+15)/16,1);clearBindings();
        copy(outRead,out);waitForGpu();
        ReadMapping mapped(context.Get(),outRead);std::memcpy(frame.pixels.data(),mapped.mapped.pData,frame.pixels.size());
        activity.downloadedBytes+=frame.pixels.size();++activity.referenceFilters;
    }
    void sourceStatistics(const Frame& source,uint32_t cell,int mode,std::span<GpuSourceStatistics> output){
        initialize();
        ensure(stats,uint32_t(output.size()),sizeof(GpuSourceStatistics),true);ensure(statsRead,uint32_t(output.size()),sizeof(GpuSourceStatistics),false,true);
        uploadSource(source);Parameters p{};
        p.image[0]=source.width;p.image[1]=source.height;p.level=uint32_t(mode);p.restoration[0]=cell;
        p.restoration[2]=(source.width+cell-1)/cell;p.restoration[3]=(source.height+cell-1)/cell;parameters(p);
        auto* srv=rgba.srv.Get();auto* uav=stats.uav.Get();context->CSSetShaderResources(1,1,&srv);context->CSSetUnorderedAccessViews(3,1,&uav,nullptr);
        context->CSSetShader(shaders[4].Get(),nullptr,0);context->Dispatch((p.restoration[2]+7)/8,(p.restoration[3]+7)/8,1);clearBindings();
        copy(statsRead,stats);D3D11_MAPPED_SUBRESOURCE mapped{};
        waitForGpu();checked(context->Map(statsRead.resource.Get(),0,D3D11_MAP_READ,0,&mapped),"Read GPU source statistics");
        std::memcpy(output.data(),mapped.pData,output.size_bytes());context->Unmap(statsRead.resource.Get(),0);
        activity.downloadedBytes+=output.size_bytes();++activity.analyses;
    }
    void decodeNative(std::span<GpuPlane> planes,const GpuNativeSettings& settings,std::span<uint8_t> output,bool allowSparse){
        TimingTimeline timing;initialize();
        // One upload of coefficient planes. All levels, CfL means, guided chroma
        // and Oklab->RGBA stay resident; only packed RGBA is copied back.
        uint32_t total=0;for(const auto& p:planes)total+=uint32_t(p.pixels.size());
        const bool uploaded=allowSparse&&uploadSparseCoefficients(planes,total);
        transform(planes,true,0,6,false,settings.cleanup,uploaded);Parameters p{};uint32_t offset=0;
        timing.mark("decode.gpu.native.inverse_submit");
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
        if(settings.chroma420&&settings.guide&&settings.chromaCleanup&&settings.chromaCleanupStrength){
            p.unused=settings.chromaCleanup;p.level=std::bit_cast<uint32_t>(settings.chromaCleanupStrength/100.f);parameters(p);
            for(uint32_t axis=0;axis<2;++axis){
                // Retain luma, padding and untouched planes on the device.
                // No coefficient or float-plane readback is introduced.
                copy(waveB,waveA);
                auto* source=waveA.srv.Get();auto* guide=guides.srv.Get();auto* target=waveB.uav.Get();
                context->CSSetShaderResources(0,1,&source);context->CSSetShaderResources(8,1,&guide);
                context->CSSetUnorderedAccessViews(0,1,&target,nullptr);context->CSSetShader(shaders[34+axis].Get(),nullptr,0);
                const uint32_t lines=((axis?settings.width:settings.height)+1)/2;
                context->Dispatch((lines+63)/64,1,1);clearBindings();
                std::swap(waveA,waveB);
            }
            timing.mark("decode.gpu.native.chroma_cleanup_submit");
        }
        ID3D11ShaderResourceView* srvs[9]{waveA.srv.Get(),nullptr,nullptr,nullptr,nullptr,gamma.srv.Get(),slopes.srv.Get(),means.srv.Get(),guides.srv.Get()};
        auto* uav=out.uav.Get();context->CSSetShaderResources(0,9,srvs);context->CSSetUnorderedAccessViews(2,1,&uav,nullptr);context->CSSetShader(shaders[7].Get(),nullptr,0);
        context->Dispatch((settings.width+15)/16,(settings.height+15)/16,1);clearBindings();
        timing.mark("decode.gpu.native.color_submit");
        copy(outRead,out);D3D11_MAPPED_SUBRESOURCE mapped{};
        waitForGpu();timing.mark("decode.gpu.native.wait");checked(context->Map(outRead.resource.Get(),0,D3D11_MAP_READ,0,&mapped),"Read GPU native decode");std::memcpy(output.data(),mapped.pData,output.size());context->Unmap(outRead.resource.Get(),0);
        timing.mark("decode.gpu.native.readback_copy");
        activity.downloadedBytes+=output.size();++activity.nativeDecodes;
        if(decodedSourceCapture&&output.size()<=32u*1024*1024){
            const auto stamp=++nextStamp;retainImage(output,stamp);
            *decodedSourceCapture={stamp,output.data(),output.size()};
        }
    }

    bool renderArtifact(std::span<const uint8_t> pixels,uint32_t width,uint32_t height,uint32_t grid,std::span<const float> sourceGuides,float contours,bool retain=false,std::span<uint8_t> resultPixels={},bool alreadyPacked=false,uint64_t sourceStamp=0){
        initialize();
        if(!artifactReciprocal.resource){
            std::array<uint32_t,4096> table;if(!nativeReciprocalTableAvx2(table))return false;
            ensure(artifactReciprocal,uint32_t(table.size()),4);upload(artifactReciprocal,table.data(),artifactReciprocal.bytes);
        }
        const auto columns=(width+grid-1)/grid;
        // Surface reconstruction is disabled here. Only twelve of the frozen
        // guide's twenty-four floats can affect this renderer. Production
        // plans pack them during CPU preparation, outside the engine lock.
        std::span<const float> packedGuides=sourceGuides;
        if(!alreadyPacked){
            const auto cells=sourceGuides.size()/24;artifactPacked.resize(cells*12);
            for(size_t i=0;i<cells;++i){const auto* source=sourceGuides.data()+i*24;auto* target=artifactPacked.data()+i*12;
                std::memcpy(target,source,10*sizeof(float));target[10]=source[20];target[11]=source[21];}
            packedGuides=artifactPacked;
        }
        ensure(artifactGuides,uint32_t(packedGuides.size()),4);upload(artifactGuides,packedGuides.data(),uint32_t(packedGuides.size_bytes()));
        ensure(artifactRowsBuffer,columns*height*12,4,true);ensure(artifactResult,width*height,4,true);if(!retain)ensure(outRead,width*height,4,false,true);
        if(!activateSource(uint32_t(pixels.size()),pixels.data(),sourceStamp))upload(rgba,pixels.data(),uint32_t(pixels.size()));
        Parameters p{};p.image[0]=width;p.image[1]=height;p.image[2]=columns;p.image[3]=grid;p.restoration[0]=std::bit_cast<uint32_t>(contours);parameters(p);
        auto* guideSource=artifactGuides.srv.Get();auto* rowTarget=artifactRowsBuffer.uav.Get();
        context->CSSetShaderResources(0,1,&guideSource);context->CSSetUnorderedAccessViews(0,1,&rowTarget,nullptr);
        context->CSSetShader(shaders[36].Get(),nullptr,0);context->Dispatch((height+63)/64,1,1);clearBindings();
        auto* rows=artifactRowsBuffer.srv.Get();auto* raw=rgba.srv.Get();auto* reciprocal=artifactReciprocal.srv.Get();auto* target=artifactResult.uav.Get();
        context->CSSetShaderResources(0,1,&rows);context->CSSetShaderResources(1,1,&raw);context->CSSetShaderResources(5,1,&reciprocal);
        context->CSSetUnorderedAccessViews(2,1,&target,nullptr);context->CSSetShader(shaders[37].Get(),nullptr,0);
        context->Dispatch((width+15)/16,(height+15)/16,1);clearBindings();++activity.contourFilters;
        if(retain)return true;
        require(resultPixels.size()==pixels.size(),"Invalid artifact output.");copy(outRead,artifactResult);waitForGpu();
        ReadMapping read(context.Get(),outRead);std::memcpy(resultPixels.data(),read.mapped.pData,pixels.size());
        activity.downloadedBytes+=pixels.size();return true;
    }
    void contourCleanup(std::span<uint8_t> target,std::span<const uint8_t> source,uint32_t width,uint32_t height,
                        float amount,float stronger,float curvature,std::span<const uint8_t> skipTiles,bool fused,bool retain=false,Buffer* resident=nullptr){
        initialize();const uint32_t columns=(width+95)/96,rows=(height+95)/96;
        const bool weak=amount<=.2f&&stronger==0&&curvature<=.06f;
        Buffer& filtered=retain?contourResult:out;
        if(!resident)activateSource(uint32_t(source.size()),source.data(),0);ensure(filtered,width*height,4,true);
        if(!retain)ensure(outRead,width*height,4,false,true);
        // Packed bytes only cross PCIe; recursive float intermediates remain on
        // the device. Do not reuse the immutable encode source cache here.
        if(!resident)upload(rgba,source.data(),uint32_t(source.size()));
        Parameters p{};p.image[0]=width;p.image[1]=height;
        p.restoration[0]=std::bit_cast<uint32_t>(amount);p.restoration[1]=std::bit_cast<uint32_t>(stronger);
        p.restoration[2]=std::bit_cast<uint32_t>(curvature);p.restoration[3]=std::bit_cast<uint32_t>(1.f/(32+stronger*20));
        p.options[1]=columns;p.options[2]=fused;p.options[3]=!skipTiles.empty();parameters(p);
        if(!weak){
            ensure(temp,columns*rows*12544,16,true);
            if(!skipTiles.empty()){std::vector<uint32_t> mask(skipTiles.begin(),skipTiles.end());ensure(slopes,uint32_t(mask.size()),4);upload(slopes,mask.data(),slopes.bytes);}
            for(uint32_t shader:{14u,15u}){
                auto* raw=(resident?resident:&rgba)->srv.Get();auto* mask=skipTiles.empty()?nullptr:slopes.srv.Get();auto* scratch=temp.uav.Get();
                context->CSSetShaderResources(1,1,&raw);context->CSSetShaderResources(6,1,&mask);context->CSSetUnorderedAccessViews(1,1,&scratch,nullptr);
                context->CSSetShader(shaders[shader].Get(),nullptr,0);context->Dispatch(columns*rows,7,1);clearBindings();
            }
        }
        auto* raw=(resident?resident:&rgba)->srv.Get();auto* scratch=weak?nullptr:temp.srv.Get();auto* mask=skipTiles.empty()?nullptr:slopes.srv.Get();auto* result=filtered.uav.Get();
        context->CSSetShaderResources(1,1,&raw);context->CSSetShaderResources(4,1,&scratch);context->CSSetShaderResources(6,1,&mask);context->CSSetUnorderedAccessViews(2,1,&result,nullptr);
        context->CSSetShader(shaders[weak?17:16].Get(),nullptr,0);context->Dispatch((width+15)/16,(height+15)/16,1);clearBindings();
        if(retain){++activity.contourFilters;return;}
        copy(outRead,out);D3D11_MAPPED_SUBRESOURCE mapped{};
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
        copy(outRead,out);D3D11_MAPPED_SUBRESOURCE mapped{};
        waitForGpu();checked(context->Map(outRead.resource.Get(),0,D3D11_MAP_READ,0,&mapped),"Read GPU resample");
        std::memcpy(output.data(),mapped.pData,output.size());context->Unmap(outRead.resource.Get(),0);
        activity.downloadedBytes+=output.size();++activity.resamples;
        retainResampled(output);
    }
};
Engine& engine(){static Engine instance;return instance;}
}
ScopedGpuDecodedSource::ScopedGpuDecodedSource(GpuDecodedSource* capture):previous_(decodedSourceCapture){decodedSourceCapture=capture;if(capture)*capture={};}
ScopedGpuDecodedSource::~ScopedGpuDecodedSource(){decodedSourceCapture=previous_;}
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
bool gpuArtifactSupported(){
    if(computeBackend()!=ComputeBackend::Gpu||!hasAvx2())return false;
    static const bool supported=[] {std::array<uint32_t,4096> table;return nativeReciprocalTableAvx2(table);}();
    return supported;
}
bool gpuArtifactCleanup(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t grid,std::span<const float> guides,float contours){
    require(pixels.size()==pixelBytes(width,height)&&(grid==8||grid==16)&&std::isfinite(contours)&&contours>=0&&contours<=2,"Invalid GPU artifact renderer.");
    const uint64_t columns=(width+grid-1)/grid,rows=(height+grid-1)/grid;
    require(guides.size()==columns*rows*24,"Invalid GPU artifact guides.");
    if(computeBackend()!=ComputeBackend::Gpu||!hasAvx2()||columns*height*12*4>256ull*1024*1024)return false;
    auto& e=engine();std::lock_guard lock(e.mutex);
    try{return e.renderArtifact(pixels,width,height,grid,guides,contours,false,pixels);}catch(const std::exception& error){e.fail(error);return false;}
}
bool gpuCanonicalInverse(std::span<GpuPlane> planes,int first,int count){
    if(computeBackend()!=ComputeBackend::Gpu)return false;
    require(!planes.empty()&&planes.size()<=3&&first>=0&&count>0&&first+count<=6,"Invalid canonical GPU transform.");
    for(const auto& p:planes)require(p.width>=64&&p.height>=64&&p.width%64==0&&p.height%64==0&&p.pixels.size()==size_t(p.width)*p.height&&p.width<=planes[0].width&&p.height<=planes[0].height,"Invalid canonical GPU plane.");
    auto& e=engine();std::lock_guard lock(e.mutex);
    try{e.initialize();if(!e.doubles)return false;e.transform(planes,true,first,count,true,0,false,true);return true;}
    catch(const std::exception& error){e.fail(error);return false;}
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
                      float sharpness,float amount,float stronger,float curvature,std::span<const uint8_t> skipTiles,bool fused,uint32_t sharpening,const ArtifactGuidePlan* artifact){
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(source.width)*source.height<512*512)return false;
    source.validate();
    require(width>=source.width&&height>=source.height&&output.size()==pixelBytes(width,height)&&
        xLow.size()==width&&xFraction.size()==width&&yLow.size()==height&&yFraction.size()==height&&
        std::isfinite(sharpness)&&sharpness>=0&&sharpness<=1.5f&&amount>0&&amount<=1&&
        stronger>=0&&stronger<=1&&curvature>=0&&curvature<=1&&sharpening<=200,"Invalid GPU contour upscale.");
    const uint64_t tiles=uint64_t((source.width+95)/96)*((source.height+95)/96);
    require(skipTiles.empty()||skipTiles.size()==tiles,"Invalid GPU contour upscale mask.");
    if(tiles*12544*16>256ull*1024*1024||tiles>65535)return false;
    if(artifact){
        require(artifact->width==source.width&&artifact->height==source.height&&(artifact->grid==8||artifact->grid==16),"Invalid combined artifact geometry.");
        const uint64_t columns=(source.width+artifact->grid-1)/artifact->grid,rows=(source.height+artifact->grid-1)/artifact->grid;
        require(artifact->guides.size()==columns*rows*24&&artifact->gpuGuides.size()==columns*rows*12,"Invalid combined artifact guides.");
        if(columns*source.height*12*4>256ull*1024*1024)return false;
    }
    TimingTimeline timing;auto& e=engine();std::lock_guard lock(e.mutex);timing.mark("decode.gpu.contour.lock");
    try{
        if(artifact&&!e.renderArtifact(source.pixels,source.width,source.height,artifact->grid,artifact->gpuGuides,artifact->contours,true,{},true,artifact->sourceStamp))return false;
        e.contourCleanup({},source.pixels,source.width,source.height,amount,stronger,curvature,skipTiles,fused,true,artifact?&e.artifactResult:nullptr);
        timing.mark("decode.gpu.contour.prepare_submit");
        e.edgeUpscale(source,width,height,output,xLow,xFraction,yLow,yFraction,sharpness,&e.contourResult,sharpening);return true;
    }catch(const std::exception& error){e.fail(error);return false;}
}
bool gpuSourceStatistics(const Frame& source,uint32_t cell,int mode,std::span<GpuSourceStatistics> output){
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(source.width)*source.height<512*512)return false;
    source.validate();require((cell==8||cell==16)&&output.size()==size_t((source.width+cell-1)/cell)*((source.height+cell-1)/cell),"Invalid GPU source analysis.");
    auto& e=engine();std::lock_guard lock(e.mutex);try{e.sourceStatistics(source,cell,mode,output);return true;}catch(const std::exception& error){e.fail(error);return false;}
}
bool gpuDecodeNative(std::span<GpuPlane> planes,const GpuNativeSettings& settings,std::span<uint8_t> output,bool allowSparse){
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(settings.width)*settings.height<512*512)return false;
    require(planes.size()==3&&output.size()==pixelBytes(settings.width,settings.height),"Invalid GPU native decode.");
    require(settings.chromaCleanup<=3&&settings.chromaCleanupStrength<=100,"Invalid GPU native color cleanup.");
    for(const auto& p:planes)require(p.width>=64&&p.height>=64&&p.width%64==0&&p.height%64==0&&
        p.pixels.size()==size_t(p.width)*p.height&&p.width<=planes[0].width&&p.height<=planes[0].height,"Invalid GPU native plane.");
    require(settings.width<=planes[0].width&&settings.height<=planes[0].height,"GPU native output exceeds its planes.");
    require(settings.cflMap.empty()||(settings.tile>=16&&settings.tile<=128&&settings.cflMap.size()==size_t((settings.width+settings.tile-1)/settings.tile)*((settings.height+settings.tile-1)/settings.tile)*2),"Invalid GPU CfL model.");
    TimingTimeline timing;auto& e=engine();std::lock_guard lock(e.mutex);timing.mark("decode.gpu.native.lock");try{e.decodeNative(planes,settings,output,allowSparse);return true;}catch(const std::exception& error){e.fail(error);return false;}
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
    require(pixels.size()==pixelBytes(width,height)&&strength<=200,"Invalid GPU final sharpening.");
    if(!strength)return true;
    auto& e=engine();std::lock_guard lock(e.mutex);
    try{e.finalSharpen(pixels,width,height,strength);return true;}catch(const std::exception& error){e.fail(error);return false;}
}
bool gpuReferenceFilter(Frame& frame,uint32_t step,std::span<const uint8_t> keep,bool adjacent){
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(frame.width)*frame.height<512*512)return false;
    frame.validate();require(step>=1&&step<=256&&keep.size()==size_t((frame.width+7)/8)*((frame.height+7)/8),"Invalid GPU reference filter.");
    if(step<4)return true;
    auto& e=engine();std::lock_guard lock(e.mutex);
    try{e.referenceFilter(frame,step,keep,adjacent);return true;}catch(const std::exception& error){e.fail(error);return false;}
}
bool gpuMotionWarp(const Frame& source,uint32_t cell,std::span<const uint32_t> vectors,const std::array<int,3>& bias,std::span<const uint8_t> modes,std::span<uint8_t> output,bool independentBlocks,std::span<const uint32_t> affine,const std::function<void()>& whilePending){
    source.validate();require((cell==16||cell==32)&&vectors.size()==size_t((source.width+cell-1)/cell)*((source.height+cell-1)/cell)&&output.size()==source.pixels.size(),"Invalid GPU motion geometry.");
    require(modes.empty()||(modes.size()==size_t((source.width+7)/8)*((source.height+7)/8)&&std::all_of(modes.begin(),modes.end(),[](uint8_t v){return v<=3;})),"Invalid GPU motion modes.");
    require(affine.empty()||(independentBlocks&&cell==32&&affine.size()==vectors.size()),"Invalid GPU affine field.");
    for(auto b:bias)require(b>=-32&&b<=32,"Invalid GPU motion bias.");
    for(auto v:vectors){const auto x=std::bit_cast<int16_t>(uint16_t(v)),y=std::bit_cast<int16_t>(uint16_t(v>>16));require(x>=-512&&x<=512&&y>=-512&&y<=512,"Invalid GPU motion vector.");}
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(source.width)*source.height<512*512)return false;
    auto& e=engine();std::exception_ptr cpuError;bool success=false;
    // Do not classify a CPU failure as a recoverable GPU failure: an inverse
    // transform may have partially modified its planes and must not be retried.
    const std::function<void()> work=whilePending?std::function<void()>([&]{try{whilePending();}catch(...){cpuError=std::current_exception();}}):std::function<void()>{};
    {std::lock_guard lock(e.mutex);
        try{e.motionWarp(source,cell,vectors,bias,modes,output,independentBlocks,affine,work);success=true;}
        catch(const std::exception& error){e.fail(error);}}
    if(cpuError)std::rethrow_exception(cpuError);return success;
}
bool gpuReconstructMotion(const Frame& source,std::span<const uint32_t> vectors,const std::array<int,3>& bias,std::span<const uint8_t> modes,std::span<const uint32_t> affine,std::span<GpuPlane> planes,uint32_t filterStep,std::span<uint8_t> output,const std::function<void()>& whilePending,bool coefficients,bool adjacentFilter){
    source.validate();const auto w=(source.width+127)&~127u,h=(source.height+127)&~127u;
    require(planes.size()==3&&filterStep<=256&&output.size()==source.pixels.size(),"Invalid GPU motion reconstruction.");
    for(unsigned c=0;c<3;++c)require(planes[c].width==(c?w/2:w)&&planes[c].height==(c?h/2:h)&&planes[c].pixels.size()==size_t(planes[c].width)*planes[c].height,"Invalid GPU residual plane.");
    require(vectors.size()==size_t((source.width+31)/32)*((source.height+31)/32)&&(affine.empty()||affine.size()==vectors.size()),"Invalid GPU reconstruction motion.");
    require(modes.size()==size_t((source.width+7)/8)*((source.height+7)/8)&&std::all_of(modes.begin(),modes.end(),[](uint8_t v){return v<=3;}),"Invalid GPU reconstruction modes.");
    for(auto b:bias)require(b>=-32&&b<=32,"Invalid GPU reconstruction bias.");
    for(auto v:vectors){const auto x=std::bit_cast<int16_t>(uint16_t(v)),y=std::bit_cast<int16_t>(uint16_t(v>>16));require(x>=-512&&x<=512&&y>=-512&&y<=512,"Invalid GPU reconstruction vector.");}
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(source.width)*source.height<512*512)return false;
    auto& e=engine();std::exception_ptr cpuError;bool success=false;
    const std::function<void()> work=whilePending?std::function<void()>([&]{try{whilePending();}catch(...){cpuError=std::current_exception();throw;}}):std::function<void()>{};
    {std::lock_guard lock(e.mutex);
        try{e.initialize();const bool gpuInverse=coefficients&&e.doubles;if(coefficients&&!gpuInverse&&!whilePending)return false;e.reconstructMotion(source,vectors,bias,modes,affine,planes,filterStep,output,work,gpuInverse,adjacentFilter);success=true;}
        catch(const std::exception& error){if(!cpuError)e.fail(error);}
        catch(...){if(!cpuError)throw;}}
    if(cpuError)std::rethrow_exception(cpuError);return success;
}
bool gpuBlockMotion(const Frame& current,const Frame& reference,std::span<uint32_t> vectors,std::span<uint32_t> affine){
    current.validate();reference.validate();require(current.width==reference.width&&current.height==reference.height&&vectors.size()==size_t((current.width+31)/32)*((current.height+31)/32),"Invalid GPU block motion geometry.");
    require(affine.empty()||affine.size()==vectors.size(),"Invalid GPU affine output.");
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(current.width)*current.height<512*512)return false;
    auto& e=engine();std::lock_guard lock(e.mutex);try{e.blockMotion(current,reference,vectors,affine);return true;}catch(const std::exception& error){e.fail(error);return false;}
}
bool gpuPredictiveWavelet(const Frame& current,const Frame& predicted,const Frame& witness,std::span<const uint8_t> keep,uint32_t propagatedError,std::span<GpuPlane> planes,bool lumaErrorOnly){
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(current.width)*current.height<512*512)return false;
    current.validate();predicted.validate();witness.validate();
    require(current.width==predicted.width&&current.height==predicted.height&&current.width==witness.width&&current.height==witness.height&&propagatedError<=32&&
            keep.size()==size_t((current.width+7)/8)*((current.height+7)/8)&&std::all_of(keep.begin(),keep.end(),[](auto value){return value<=3;}),"Invalid GPU predictive inputs.");
    require(planes.size()==3,"Invalid GPU predictive planes.");
    const uint32_t w=(current.width+127)&~127u,h=(current.height+127)&~127u;
    for(unsigned c=0;c<3;++c)require(planes[c].width==(c?w/2:w)&&planes[c].height==(c?h/2:h)&&planes[c].pixels.size()==size_t(planes[c].width)*planes[c].height,"Invalid GPU predictive plane geometry.");
    auto& e=engine();std::lock_guard lock(e.mutex);try{e.predictiveWavelet(current,predicted,witness,keep,propagatedError,planes,lumaErrorOnly);return true;}catch(const std::exception& error){e.fail(error);return false;}
}
bool gpuPreparePredictive(const Frame& current,const Frame& sourceReference,const Frame& decodedReference,
                          std::span<const uint32_t> vectors,std::span<const uint32_t> affine,const std::array<int,3>& bias,
                          uint32_t tolerance,uint32_t propagatedError,std::span<uint8_t> predicted,std::span<uint8_t> keep,std::span<GpuPlane> planes,bool lumaErrorOnly){
    if(computeBackend()!=ComputeBackend::Gpu||uint64_t(current.width)*current.height<512*512)return false;
    current.validate();sourceReference.validate();decodedReference.validate();
    require(current.width==sourceReference.width&&current.height==sourceReference.height&&current.width==decodedReference.width&&current.height==decodedReference.height&&
            predicted.size()==current.pixels.size()&&tolerance<=16&&propagatedError<=32&&planes.size()==3&&
            vectors.size()==size_t((current.width+31)/32)*((current.height+31)/32)&&affine.size()==vectors.size()&&
            keep.size()==size_t((current.width+7)/8)*((current.height+7)/8),"Invalid GPU predictive preparation.");
    for(auto b:bias)require(b>=-32&&b<=32,"Invalid GPU motion bias.");
    for(auto v:vectors){const auto x=std::bit_cast<int16_t>(uint16_t(v)),y=std::bit_cast<int16_t>(uint16_t(v>>16));require(x>=-512&&x<=512&&y>=-512&&y<=512,"Invalid GPU motion vector.");}
    const uint32_t w=(current.width+127)&~127u,h=(current.height+127)&~127u;
    for(unsigned c=0;c<3;++c)require(planes[c].width==(c?w/2:w)&&planes[c].height==(c?h/2:h)&&planes[c].pixels.size()==size_t(planes[c].width)*planes[c].height,"Invalid GPU predictive plane geometry.");
    auto& e=engine();std::lock_guard lock(e.mutex);
    try{e.preparePredictive(current,sourceReference,decodedReference,vectors,affine,bias,tolerance,propagatedError,predicted,keep,planes,lumaErrorOnly);return true;}
    catch(const std::exception& error){e.fail(error);return false;}
}

}
