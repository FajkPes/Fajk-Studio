#include "reconstruction441_gpu.hpp"
#include "reconstruction441_shaders.hpp"
#include "flat_regions.hpp"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <array>
#include <mutex>
#include <cstring>
#include <sstream>
namespace fajk {
namespace {
using Microsoft::WRL::ComPtr;
void checked(HRESULT hr) { if(FAILED(hr))throw std::runtime_error("R441 GPU operation failed."); }
struct Surface {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};
struct Gpu {
    std::mutex mutex;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    std::array<ComPtr<ID3D11ComputeShader>,13> shaders;
    std::array<Surface,7> images;
    Surface source,packed,alternate,tiles,low,flat;
    uint32_t lowWidth=0,lowHeight=0,flatWidth=0,flatHeight=0;
    ComPtr<ID3D11Texture2D> staging;
    uint32_t width=0,height=0;
    bool unavailable=false;
    std::string name;
    std::string profile;
    std::array<ComPtr<ID3D11Query>,32> stamps;
    ComPtr<ID3D11Query> disjoint;
    unsigned stampCount=0;
    std::vector<std::string> stageNames;
    bool diagnostics=false;
    void stamp(const char* stage){if(!diagnostics)return;context->End(stamps[stampCount++].Get());stageNames.emplace_back(stage);}
    void initialize() {
        if(device&&shaders.back())return;
        D3D_FEATURE_LEVEL level;
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        checked(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,1,D3D11_SDK_VERSION,&device,&level,&context));
        ComPtr<IDXGIDevice> dxgi;checked(device.As(&dxgi));ComPtr<IDXGIAdapter> adapter;checked(dxgi->GetAdapter(&adapter));
        DXGI_ADAPTER_DESC desc{};checked(adapter->GetDesc(&desc));
        for(auto c:desc.Description){if(!c)break;name+=c<128?char(c):'?';}
        const char* entries[]{"initialize","horizontal","vertical","monotone","tensor","direction","coherent","finish","antialias","contrast","textMask","textFinish","upscale"};
        for(size_t i=0;i<shaders.size();++i) {
            ComPtr<ID3DBlob> code,errors;
            const auto hr=D3DCompile(reconstruction441Shaders,sizeof(reconstruction441Shaders)-1,"R441",nullptr,nullptr,entries[i],"cs_5_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,&code,&errors);
            if(FAILED(hr))throw std::runtime_error(errors?static_cast<const char*>(errors->GetBufferPointer()):"Cannot compile R441 shader");
            checked(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shaders[i]));
        }
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=48;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        checked(device->CreateBuffer(&cb,nullptr,&constants));
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        checked(device->CreateSamplerState(&sd,&sampler));
        D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP,0};for(auto& stamp:stamps)checked(device->CreateQuery(&q,&stamp));
        q.Query=D3D11_QUERY_TIMESTAMP_DISJOINT;checked(device->CreateQuery(&q,&disjoint));
    }
    Surface surface(DXGI_FORMAT format,bool writable=true) {
        Surface s;D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=1;d.Format=format;
        d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|(writable?D3D11_BIND_UNORDERED_ACCESS:0);
        checked(device->CreateTexture2D(&d,nullptr,&s.texture));checked(device->CreateShaderResourceView(s.texture.Get(),nullptr,&s.srv));
        if(writable)checked(device->CreateUnorderedAccessView(s.texture.Get(),nullptr,&s.uav));return s;
    }
    void resize(uint32_t w,uint32_t h) {
        if(width==w&&height==h&&staging)return;
        for(auto& image:images)image={};source={};packed={};alternate={};tiles={};staging.Reset();width=w;height=h;
        for(auto& image:images)image=surface(DXGI_FORMAT_R16G16B16A16_FLOAT);
        source=surface(DXGI_FORMAT_R8G8B8A8_UINT);packed=surface(DXGI_FORMAT_R8G8B8A8_UINT);alternate=surface(DXGI_FORMAT_R8G8B8A8_UINT);
        width=(w+15)/16;height=(h+15)/16;tiles=surface(DXGI_FORMAT_R32G32B32A32_UINT);width=w;height=h;
        D3D11_TEXTURE2D_DESC d{};source.texture->GetDesc(&d);d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        checked(device->CreateTexture2D(&d,nullptr,&staging));
    }
    void run(unsigned shader,int input,int output,int guide=-1,int direction=-1) {
        ID3D11ShaderResourceView* srvs[]{input>=0?images[input].srv.Get():nullptr,guide>=0?images[guide].srv.Get():nullptr,
                                      direction>=0?images[direction].srv.Get():nullptr,source.srv.Get()};
        ID3D11UnorderedAccessView* uavs[]{output>=0?images[output].uav.Get():nullptr,output<0?packed.uav.Get():nullptr};
        context->CSSetShaderResources(0,4,srvs);context->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
        context->CSSetShader(shaders[shader].Get(),nullptr,0);context->Dispatch((width+15)/16,(height+7)/8,1);
        const char* labels[]{"init","blurH","blurV","skirt","tensor","directions","tangents","finish"};stamp(labels[shader]);
        ID3D11ShaderResourceView* emptySrv[4]{};ID3D11UnorderedAccessView* emptyUav[2]{};
        context->CSSetShaderResources(0,4,emptySrv);context->CSSetUnorderedAccessViews(0,2,emptyUav,nullptr);
    }
    void packedPass(unsigned shader,const Surface& raw,const Surface* candidate,const Surface& target) {
        ID3D11ShaderResourceView* srvs[]{nullptr,nullptr,nullptr,raw.srv.Get(),candidate?candidate->srv.Get():nullptr,shader==11?tiles.srv.Get():nullptr};
        ID3D11UnorderedAccessView* uavs[]{nullptr,shader==10?nullptr:target.uav.Get(),shader==10?tiles.uav.Get():nullptr};
        context->CSSetShaderResources(0,6,srvs);context->CSSetUnorderedAccessViews(0,3,uavs,nullptr);
        context->CSSetShader(shaders[shader].Get(),nullptr,0);
        context->Dispatch((width+15)/16,(height+(shader==10?15:7))/(shader==10?16:8),1);
        stamp(shader==8?"AA":shader==9?"contrast":shader==10?"textMask":"textFinish");
        ID3D11ShaderResourceView* emptySrv[6]{};ID3D11UnorderedAccessView* emptyUav[3]{};
        context->CSSetShaderResources(0,6,emptySrv);context->CSSetUnorderedAccessViews(0,3,emptyUav,nullptr);
    }
    void reconstruct(std::span<const uint8_t> input,uint32_t iw,uint32_t ih,std::span<uint8_t> pixels,uint32_t w,uint32_t h,float upscale,const FlatRegionLayout* layout,float amount,float stronger,float trim,float aa,float contrast,bool protectText) {
        initialize();resize(w,h);
        stageNames.clear();stampCount=0;if(diagnostics)context->Begin(disjoint.Get());stamp("start");
        struct Settings{uint32_t w,h;float amount,stronger,trim,aa,contrast;uint32_t protectText,iw,ih;float upscale;uint32_t tile;}
            settings{w,h,amount,stronger,trim,aa,contrast,protectText,iw,ih,upscale,layout?layout->tile:0};
        context->UpdateSubresource(constants.Get(),0,nullptr,&settings,0,0);
        ID3D11Buffer* cb=constants.Get();context->CSSetConstantBuffers(0,1,&cb);
        ID3D11SamplerState* ss=sampler.Get();context->CSSetSamplers(0,1,&ss);
        if(iw!=w||ih!=h){
            if(lowWidth!=iw||lowHeight!=ih){width=iw;height=ih;low=surface(DXGI_FORMAT_R8G8B8A8_UINT,false);lowWidth=iw;lowHeight=ih;width=w;height=h;}
            context->UpdateSubresource(low.texture.Get(),0,nullptr,input.data(),iw*4,0);
            if(layout){
                const uint32_t cols=layout->cols,rows=(h+layout->tile-1)/layout->tile;
                if(flatWidth!=cols||flatHeight!=rows){width=cols;height=rows;flat=surface(DXGI_FORMAT_R8G8B8A8_UINT,false);flatWidth=cols;flatHeight=rows;width=w;height=h;}
                std::vector<uint32_t> colors(layout->map.size());for(size_t i=0;i<colors.size();++i)colors[i]=layout->palette[layout->map[i]];
                context->UpdateSubresource(flat.texture.Get(),0,nullptr,colors.data(),cols*4,0);
            }
            ID3D11ShaderResourceView* inputs[7]{nullptr,nullptr,nullptr,low.srv.Get(),nullptr,nullptr,layout?flat.srv.Get():nullptr};
            ID3D11UnorderedAccessView* outputs[2]{nullptr,source.uav.Get()};
            context->CSSetShaderResources(0,7,inputs);context->CSSetUnorderedAccessViews(0,2,outputs,nullptr);
            context->CSSetShader(shaders[12].Get(),nullptr,0);context->Dispatch((w+15)/16,(h+7)/8,1);
            stamp("upscale");
            ID3D11ShaderResourceView* noInputs[7]{};ID3D11UnorderedAccessView* noOutputs[2]{};
            context->CSSetShaderResources(0,7,noInputs);context->CSSetUnorderedAccessViews(0,2,noOutputs,nullptr);
        }else context->UpdateSubresource(source.texture.Get(),0,nullptr,input.data(),w*4,0);
        run(0,-1,0);
        auto guide=[&](int input){run(1,input,3);run(2,3,2);run(1,2,3);run(2,3,2);};
        guide(0);run(3,0,1,2);
        guide(1);run(4,2,4);run(1,4,3);run(2,3,2);run(5,2,5,1);
        run(6,1,0,-1,5);run(6,0,6,-1,5);run(6,6,0,-1,5);run(6,0,6,-1,5);run(7,6,-1,-1,5);
        Surface* current=&packed;Surface* next=&alternate;
        if(aa>0){packedPass(8,*current,nullptr,*next);std::swap(current,next);}
        if(contrast>0){packedPass(9,*current,nullptr,*next);std::swap(current,next);}
        if(protectText){packedPass(10,source,nullptr,*next);packedPass(11,source,current,*next);std::swap(current,next);}
        context->CopyResource(staging.Get(),current->texture.Get());
        stamp("readback");if(diagnostics)context->End(disjoint.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};checked(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
        for(uint32_t y=0;y<h;++y)std::memcpy(pixels.data()+size_t(y)*w*4,static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,w*4);
        context->Unmap(staging.Get(),0);
        if(diagnostics){D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
            if(context->GetData(disjoint.Get(),&d,sizeof(d),0)==S_OK&&!d.Disjoint){std::ostringstream out;uint64_t previous=0;
                for(unsigned i=0;i<stampCount;++i){uint64_t at=0;checked(context->GetData(stamps[i].Get(),&at,sizeof(at),0));if(i)out<<stageNames[i]<<' '<<double(at-previous)*1000/d.Frequency<<" ms\n";previous=at;}profile=out.str();}}
    }
};
Gpu& gpu(){static Gpu instance;return instance;}
}
bool reconstruct441Gpu(std::span<uint8_t> pixels,uint32_t w,uint32_t h,float amount,float stronger,float trim,float aa,float contrast,bool protectText) {
    auto& g=gpu();std::lock_guard lock(g.mutex);if(g.unavailable)return false;
    try{g.reconstruct(pixels,w,h,pixels,w,h,1,nullptr,amount,stronger,trim,aa,contrast,protectText);return true;}
    catch(const std::exception& e){g.unavailable=true;g.name+=std::string(" ERROR: ")+e.what();return false;}
}
std::string reconstruction441GpuDevice(){auto& g=gpu();std::lock_guard lock(g.mutex);return g.name;}
void reconstruction441GpuDiagnostics(bool enabled){auto& g=gpu();std::lock_guard lock(g.mutex);g.diagnostics=enabled;}
std::string reconstruction441GpuProfile(){auto& g=gpu();std::lock_guard lock(g.mutex);return g.profile;}
bool reconstruct441GpuScaled(std::span<const uint8_t> input,uint32_t iw,uint32_t ih,std::span<uint8_t> output,uint32_t w,uint32_t h,float upscale,const FlatRegionLayout* flat,float amount,float stronger,float trim,float aa,float contrast,bool protectText){
    auto& g=gpu();std::lock_guard lock(g.mutex);if(g.unavailable)return false;
    try{g.reconstruct(input,iw,ih,output,w,h,upscale,flat,amount,stronger,trim,aa,contrast,protectText);return true;}
    catch(const std::exception& e){g.unavailable=true;g.name+=std::string(" ERROR: ")+e.what();return false;}
}
}

