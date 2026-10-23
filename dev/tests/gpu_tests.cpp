#include "fajk/codec.hpp"
#include "../src/gpu_compute.hpp"
#include "../src/wavelet.hpp"
#include "../src/resample.hpp"
#include "../src/source_priority.hpp"
#include "../src/fast_reconstruction.hpp"
#include "../src/color_prediction_fit.hpp"
#include "../src/color_prediction_rows.hpp"
#include <iostream>
#include <random>
#include <future>
#include <cmath>
#include <cstring>
using namespace fajk;
namespace fajk {void fastEdgeUpscaleRow(const uint8_t*,uint32_t,uint32_t,uint8_t*,uint32_t,const int*,const float*,int,float,float);}
void check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
int main(){try{
    const auto info=gpuInfo();
    if(!info.available){std::cout<<"SKIP GPU: "<<info.error<<'\n';return info.device.empty()?77:1;}
    std::cout<<"GPU: "<<info.device<<'\n';
    std::mt19937 rng(470);
    double worst=0;
    for(auto [w,h]:{std::pair{256,256},std::pair{320,448},std::pair{1024,768}}){
        for(bool inverse:{false,true})for(int first:{0,1}){
            std::vector<float> input(size_t(w)*h);
            for(auto& v:input)v=float(int(rng()%100001)-50000)*.002f;
            auto cpu=input,gpu=input;
            waveletTransform(cpu,w,h,3,inverse,true,true,first,6-first);
            {
                ScopedComputeBackend scope(ComputeBackend::Gpu);
                GpuPlane plane{gpu,uint32_t(w),uint32_t(h)};
                if(uint64_t(w>>first)*(h>>first)>=256*256)check(gpuWavelet({&plane,1},inverse,first,6-first),"GPU transform unexpectedly declined.");
                else waveletTransform(gpu,w,h,3,inverse,true,true,first,6-first);
            }
            double sum=0,error=0;
            for(size_t i=0;i<cpu.size();++i){const double e=std::abs(cpu[i]-gpu[i]);error=std::max(error,e);sum+=e;}
            worst=std::max(worst,error);
            std::cout<<w<<'x'<<h<<" inverse="<<inverse<<" first="<<first<<" max="<<error<<" mean="<<sum/cpu.size()<<'\n';
            check(error<.004&&sum/cpu.size()<.0001,"GPU CDF 9/7 differs from production CPU.");
        }
    }
    for(auto [w,h]:{std::pair{512u,515u},std::pair{1024u,768u}}){
        Frame source{w,h,17,23,Bytes(pixelBytes(w,h),255)};
        for(size_t i=0;i<source.pixels.size();i+=4)for(int c=0;c<3;++c)source.pixels[i+c]=uint8_t(rng());
        const uint32_t ow=w*3/4,oh=h*3/4;
        auto cpu=resamplePhoto(source,ow,oh,3,{},2);
        Frame gpu;
        {ScopedComputeBackend scope(ComputeBackend::Gpu);gpu=resamplePhoto(source,ow,oh,3,{},2);}
        double error=0;size_t changed=0;
        for(size_t i=0;i<cpu.pixels.size();++i){const int d=std::abs(int(cpu.pixels[i])-gpu.pixels[i]);check(d<=1,"GPU resample changed rounding by more than 1.");error+=d;changed+=d!=0;}
        check(error/cpu.pixels.size()<.002,"GPU resample is not sufficiently close.");
        std::cout<<"Resample changed bytes "<<changed<<" / "<<cpu.pixels.size()<<'\n';
        auto codec=makeCodec(CodecId::Fajk);EncodeOptions o;o.threads=3;o.targetBytes=6000;
        SourcePriority cpuPriority(source,3,31);
        std::unique_ptr<SourcePriority> gpuPriority;
        {ScopedComputeBackend scope(ComputeBackend::Gpu);gpuPriority=std::make_unique<SourcePriority>(source,3,31);}
        for(uint32_t y=0;y<h;y+=13)for(uint32_t x=0;x<w;x+=17)for(uint32_t size:{8u,32u,128u}){
            check(cpuPriority.bandRate(x,y,size,size)==gpuPriority->bandRate(x,y,size,size),"GPU analysis changed source classification.");
            check(cpuPriority.flatCoverage(x,y,size,size,100)==gpuPriority->flatCoverage(x,y,size,size,100),"GPU flat coverage changed.");
            check(cpuPriority.hasDirectEdge(x,y,size,size)==gpuPriority->hasDirectEdge(x,y,size,size),"GPU analysis changed source edges.");
        }
        auto packet=codec->encode(source,o);auto baseline=codec->decode(packet,w,h);
        codec->setComputeBackend(ComputeBackend::Gpu);
        auto decoded=codec->decode(packet,w,h);
        double total=0;int maximum=0;
        for(size_t i=0;i<decoded.pixels.size();++i){const int d=std::abs(int(decoded.pixels[i])-baseline.pixels[i]);total+=d;maximum=std::max(maximum,d);}
        std::cout<<"Decode max="<<maximum<<" mean="<<total/decoded.pixels.size()<<'\n';
        check(maximum<=3&&total/decoded.pixels.size()<.02,"GPU decode quality regression.");
        o.backend=ComputeBackend::Gpu;auto coded=codec->encode(source,o);
        check(coded.data.size()<=o.targetBytes,"GPU encode exceeded budget.");
        o.budgetHeatmap=std::make_shared<BudgetHeatmap>();
        check(codec->encode(source,o).data==coded.data,"GPU debug capture changed the packet.");
        o.budgetHeatmap.reset();
        codec->setComputeBackend(ComputeBackend::Cpu);codec->decode(coded,w,h).validate();
        for(uint32_t sampling:{420u,422u,444u})for(uint32_t budget:{6000u,40000u}){
            EncodeOptions settings;settings.threads=3;settings.targetBytes=budget;settings.chroma420=sampling==420;settings.chroma422=sampling==422;
            const auto sample=codec->encode(source,settings);
            codec->setComputeBackend(ComputeBackend::Cpu);const auto a=codec->decode(sample,w,h);
            codec->setComputeBackend(ComputeBackend::Gpu);const auto b=codec->decode(sample,w,h);
            double sum=0;int maximum=0;
            for(size_t i=0;i<a.pixels.size();++i){const int d=std::abs(int(a.pixels[i])-b.pixels[i]);sum+=d;maximum=std::max(maximum,d);}
            check(maximum<=4&&sum/a.pixels.size()<.02,"GPU native filter/chroma quality regression.");
        }
        // Interleave different-sized jobs, then reuse the original tap cache.
        // Cached resampler geometry must not reuse resized analysis/decode buffers.
        Frame again;
        {ScopedComputeBackend scope(ComputeBackend::Gpu);again=resamplePhoto(source,ow,oh,3,{},2);}
        check(again.pixels==gpu.pixels,"GPU buffer cache was corrupted by another operation.");
    }
    // Compare the new recursive postfilter against its CPU arithmetic directly,
    // including partial tiles, transparent pixels, skip masks and both FMA modes.
    for(auto [w,h]:{std::pair{513u,515u},std::pair{768u,512u}}){
        Bytes source(pixelBytes(w,h));
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const auto at=(size_t(y)*w+x)*4;
            for(uint32_t c=0;c<3;++c)source[at+c]=uint8_t(std::clamp(int(128+100*std::sin((x+c*7)*.07+std::cos(y*.02)*4)+20*std::sin(y*.1))+int(rng()%7)-3,0,255));
            source[at+3]=(x%37==0&&y%19==0)?128:255;
        }
        Bytes skip(size_t((w+95)/96)*((h+95)/96));for(size_t i=0;i<skip.size();++i)skip[i]=i%5==0;
        for(bool fused:{false,true})for(float amount:{.1f,.8f}){
            const float stronger=amount>.2f?.3f:0,curvature=amount>.2f?.27f:0;
            const auto mask=amount>.2f?std::span<const uint8_t>(skip):std::span<const uint8_t>{};
            auto cpu=source,gpu=source;
            recursiveContourCleanup(cpu,w,h,3,amount,stronger,source,curvature,mask,fused);
            {ScopedComputeBackend scope(ComputeBackend::Gpu);check(gpuContourCleanup(gpu,source,w,h,amount,stronger,curvature,mask,fused),"GPU contour filter declined.");}
            int maximum=0;double sum=0;size_t changed=0;
            for(size_t i=0;i<cpu.size();++i){const int d=std::abs(int(cpu[i])-gpu[i]);maximum=std::max(maximum,d);sum+=d;changed+=d!=0;}
            std::cout<<"Contour "<<w<<'x'<<h<<" fused="<<fused<<" amount="<<amount<<" max="<<maximum<<" mean="<<sum/cpu.size()<<" changed="<<changed<<'\n';
            check(maximum<=1&&sum/cpu.size()<.002,"GPU recursive contour quality regression.");
            if(fused&&amount>.2f){
                auto integrated=source;const auto before=gpuActivity().contourFilters;
                {ScopedComputeBackend scope(ComputeBackend::Gpu);recursiveContourCleanup(integrated,w,h,1,amount,stronger,{},curvature,mask,fused);}
                check(integrated==gpu&&gpuActivity().contourFilters==before+1,"Recursive filter did not select GPU for a small CPU budget.");
                integrated=source;
                {ScopedComputeBackend scope(ComputeBackend::Gpu);recursiveContourCleanup(integrated,w,h,3,amount,stronger,source,curvature,mask,fused);}
                check(integrated==cpu&&gpuActivity().contourFilters==before+1,"Recursive filter added a GPU transfer with a large CPU budget.");
            }
            for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const size_t at=(size_t(y)*w+x)*4;
                check(gpu[at+3]==source[at+3],"GPU contour changed alpha.");
                if(amount>.2f&&(source[at+3]!=255||skip[size_t(y/96)*((w+95)/96)+x/96]))
                    for(uint32_t c=0;c<4;++c)check(gpu[at+c]==source[at+c],"GPU contour changed a protected pixel.");
            }
        }
    }
    // The color model uses the very same GPU-converted planes. Compare its
    // double reductions and final quantized patch equations
    // against the original CPU algorithm, including clamped partial cells.
    for(auto [w,h]:{std::pair{513u,517u},std::pair{768u,577u}}){
        Frame frame{w,h,0,1,Bytes(pixelBytes(w,h),255)};
        for(size_t at=0;at<frame.pixels.size();at+=4)for(int c=0;c<3;++c)frame.pixels[at+c]=uint8_t(rng());
        const uint32_t stride=(w+63)&~63u,ph=(h+63)&~63u;
        WaveletSourceColor source;source.width=w;source.height=h;
        for(auto& p:source.planes)p.resize(size_t(stride)*ph);
        std::array<GpuPlane,3> planes{{{source.planes[0],stride,ph},{source.planes[1],stride,ph},{source.planes[2],stride,ph}}};
        for(uint32_t tile:{32u,64u}){
            const uint32_t cols=(w+tile-1)/tile,rows=(h+tile-1)/tile;
            std::vector<GpuColorStatistics> summaries(size_t(cols)*rows);
            source.fittedPatches.resize(tile==64?size_t(cols+1)*(rows+1):0);
            bool supported=false;
            {ScopedComputeBackend scope(ComputeBackend::Gpu);supported=gpuSourceColorModel(frame,planes,tile,summaries,source.fittedPatches);}
            if(!supported){source.fittedPatches.clear();std::cout<<"SKIP optional double color model\n";continue;}
            for(uint32_t ty=0;ty<rows;++ty)for(uint32_t tx=0;tx<cols;++tx){
                GpuColorStatistics reference;
                for(uint32_t y=ty*tile;y<std::min(h,(ty+1)*tile);++y)for(uint32_t x=tx*tile;x<std::min(w,(tx+1)*tile);++x){
                    const size_t at=size_t(y)*stride+x;const double l=source.planes[0][at];reference.sum+=l;reference.square+=l*l;
                    for(int c=0;c<2;++c){const double color=source.planes[c+1][at];reference.color[c]+=color;reference.cross[c]+=l*color;}++reference.count;
                }
                const auto& s=summaries[size_t(ty)*cols+tx];
                auto close=[](double a,double b){return std::abs(a-b)<=1e-11*std::max(1.,std::abs(a));};
                check(close(reference.sum,s.sum)&&close(reference.square,s.square)&&reference.count==s.count,"GPU color reduction lost precision.");
                for(int c=0;c<2;++c)check(close(reference.color[c],s.color[c])&&close(reference.cross[c],s.cross[c]),"GPU color covariance lost precision.");
            }
            if(tile==64){
                Bytes a(size_t(cols)*rows*2);for(auto& v:a)v=uint8_t(int8_t(int(rng()%49)-24));auto b=a;
                refineInterpolatedChroma(source,tile,3,a);
                source.fittedPatches.clear();refineInterpolatedChroma(source,tile,3,b);
                check(a==b,"GPU chroma patch statistics changed quantized Jacobi correction.");
            }
        }
    }
    // A downsampled image already resides on the GPU. Reusing that exact
    // immutable output must not upload it again for subsequent analysis.
    {
        Frame frame{1024,768,0,1,Bytes(pixelBytes(1024,768),255)};
        for(size_t at=0;at<frame.pixels.size();at+=4)frame.pixels[at]=uint8_t(rng());
        ScopedComputeBackend scope(ComputeBackend::Gpu);
        auto small=resamplePhoto(frame,768,576,3,{},2);const auto bytes=gpuActivity().uploadedBytes;
        std::vector<GpuSourceStatistics> values(size_t(48)*36);
        check(gpuSourceStatistics(small,16,0,values),"Resampled source reuse test declined.");
        check(gpuActivity().uploadedBytes==bytes,"GPU resampler uploaded its own result again.");
        auto again=resamplePhoto(frame,768,576,3,{},2);
        check(again.pixels==small.pixels,"Retained resampler buffer lost its writable view.");
    }
    // Finite edge interpolation, border clamping and the AVX2 eight-lane
    // activation must survive odd dimensions and partially filled groups.
    for(auto [iw,ih,ow,oh]:{std::array{257u,259u,513u,517u},std::array{512u,512u,768u,777u}}){
        Frame source{iw,ih,0,1,Bytes(pixelBytes(iw,ih),255)};
        std::vector<int> xl(ow),yl(oh);std::vector<float> xf(ow),yf(oh);
        for(uint32_t x=0;x<ow;++x){double p=(x+.5)*iw/ow-.5;xl[x]=int(std::floor(p));xf[x]=float(p-xl[x]);}
        for(uint32_t y=0;y<oh;++y){double p=(y+.5)*ih/oh-.5;yl[y]=int(std::floor(p));yf[y]=float(p-yl[y]);}
        for(bool smooth:{false,true})for(float sharpness:{0.f,.7f,1.5f}){
            for(uint32_t y=0;y<ih;++y)for(uint32_t x=0;x<iw;++x)for(uint32_t c=0;c<3;++c)
                source.pixels[(size_t(y)*iw+x)*4+c]=smooth?uint8_t(((x/19+y/23+c)*3+(x%17==0?80:0))%256):uint8_t(rng());
            Bytes cpu(pixelBytes(ow,oh)),gpu(cpu.size());
            for(uint32_t y=0;y<oh;++y)fastEdgeUpscaleRow(source.pixels.data(),iw,ih,cpu.data()+size_t(y)*ow*4,ow,xl.data(),xf.data(),yl[y],yf[y],sharpness);
            {ScopedComputeBackend scope(ComputeBackend::Gpu);check(gpuEdgeUpscale(source,ow,oh,gpu,xl,xf,yl,yf,sharpness),"GPU edge upscale declined.");}
            int maximum=0;double sum=0;
            for(size_t i=0;i<cpu.size();++i){int d=std::abs(int(cpu[i])-gpu[i]);maximum=std::max(maximum,d);sum+=d;}
            std::cout<<"Edge upscale "<<ow<<'x'<<oh<<" sharpness="<<sharpness<<" max="<<maximum<<" mean="<<sum/cpu.size()<<'\n';
            check(maximum<=1&&sum/cpu.size()<.00001,"GPU edge interpolation changed reconstruction.");
        }
    }
    // The joined contour/upscale chain must equal two separate GPU operations,
    // while transferring only one source and one final output (plus axes).
    {
        constexpr uint32_t iw=513,ih=515,ow=769,oh=777;
        Frame source{iw,ih,0,1,Bytes(pixelBytes(iw,ih),255)};
        for(size_t at=0;at<source.pixels.size();at+=4)for(int c=0;c<3;++c)source.pixels[at+c]=uint8_t(rng());
        const auto saved=source.pixels;
        std::vector<int> xl(ow),yl(oh);std::vector<float> xf(ow),yf(oh);
        for(uint32_t x=0;x<ow;++x){double p=(x+.5)*iw/ow-.5;xl[x]=int(std::floor(p));xf[x]=float(p-xl[x]);}
        for(uint32_t y=0;y<oh;++y){double p=(y+.5)*ih/oh-.5;yl[y]=int(std::floor(p));yf[y]=float(p-yl[y]);}
        for(bool fused:{false,true})for(float amount:{.1f,.8f}){
            ScopedComputeBackend scope(ComputeBackend::Gpu);
            const float stronger=amount>.2f?.3f:0,curvature=amount>.2f?.27f:0;
            auto intermediate=source;Bytes separate(pixelBytes(ow,oh)),joined(separate.size());
            check(gpuContourCleanup(intermediate.pixels,source.pixels,iw,ih,amount,stronger,curvature,{},fused),"Separate contour declined.");
            check(gpuEdgeUpscale(intermediate,ow,oh,separate,xl,xf,yl,yf,.8f),"Separate upscale declined.");
            const auto before=gpuActivity();
            check(gpuContourUpscale(source,ow,oh,joined,xl,xf,yl,yf,.8f,amount,stronger,curvature,{},fused),"Joined contour upscale declined.");
            const auto after=gpuActivity();
            check(joined==separate&&source.pixels==saved,"Joined GPU chain changed pixels or source.");
            check(after.uploadedBytes-before.uploadedBytes==source.pixels.size()+size_t(ow+oh)*8&&
                  after.downloadedBytes-before.downloadedBytes==joined.size(),"Joined GPU chain transferred its intermediate.");
        }
    }
    // Forward preparation consumes the exact retained GPU source, with partial
    // cells, independently padded chroma and only a small model upload.
    {
        constexpr uint32_t w=513,h=517,stride=576,ph=576,cw=320,ch=320,tile=64;
        Frame frame{w,h,0,1,Bytes(pixelBytes(w,h),255)};
        for(size_t at=0;at<frame.pixels.size();at+=4)for(int c=0;c<3;++c)frame.pixels[at+c]=uint8_t(rng());
        WaveletSourceColor source;for(auto& p:source.planes)p.resize(size_t(stride)*ph);
        std::array<GpuPlane,3> colors{{{source.planes[0],stride,ph},{source.planes[1],stride,ph},{source.planes[2],stride,ph}}};
        constexpr uint32_t cols=(w+tile-1)/tile,rows=(h+tile-1)/tile;
        std::vector<GpuColorStatistics> summaries(size_t(cols)*rows);
        ScopedComputeBackend scope(ComputeBackend::Gpu);
        if(gpuSourceColorModel(frame,colors,tile,summaries,{})){
            Bytes map(size_t(cols)*rows*2);for(auto& v:map)v=uint8_t(int8_t(int(rng()%49)-24));
            std::vector<float> means(size_t(cols)*rows);for(size_t i=0;i<means.size();++i)means[i]=float(summaries[i].sum/summaries[i].count);
            std::array<std::vector<float>,3> reference{source.planes[0],std::vector<float>(cw*ch),std::vector<float>(cw*ch)},result=reference;
            for(uint32_t y=0;y<ch;++y){
                std::array<std::vector<float>,3> model;for(auto& p:model)p.resize(cw);
                r6ModelRow(map,means,tile,cols,rows,float(std::min(h-1,y*2))+.5f,cw,2,.5f,model[0].data(),model[1].data(),model[2].data());
                for(uint32_t x=0;x<cw;++x){
                    const auto ax=std::min(w-1,x*2),bx=std::min(w-1,ax+1),ay=std::min(h-1,y*2),by=std::min(h-1,ay+1);
                    const auto& l=source.planes[0];
                    const float ac=((l[size_t(ay)*stride+ax]+l[size_t(ay)*stride+bx])+(l[size_t(by)*stride+ax]+l[size_t(by)*stride+bx]))*.25f-model[2][x];
                    for(int c=0;c<2;++c){const auto& p=source.planes[c+1];
                        reference[c+1][size_t(y)*cw+x]=((p[size_t(ay)*stride+ax]+p[size_t(ay)*stride+bx])+(p[size_t(by)*stride+ax]+p[size_t(by)*stride+bx]))*.25f-model[c][x]*ac;}
                }
            }
            for(int c=0;c<3;++c)waveletTransform(reference[c],c?cw:stride,c?ch:ph,3,false);
            std::array<GpuPlane,3> planes{{{result[0],stride,ph},{result[1],cw,ch},{result[2],cw,ch}}};
            const auto before=gpuActivity();check(gpuSourceWavelet(frame,planes,tile,map,means),"Retained source forward declined.");
            check(gpuActivity().uploadedBytes-before.uploadedBytes==means.size()*8,"Forward uploaded full CPU float planes.");
            for(int c=0;c<3;++c){double sum=0,maximum=0;for(size_t i=0;i<result[c].size();++i){double d=std::abs(result[c][i]-reference[c][i]);sum+=d;maximum=std::max(maximum,d);}
                std::cout<<"Resident forward plane="<<c<<" max="<<maximum<<" sum="<<sum<<'\n';
                check(maximum==0&&sum==0,"Resident source forward changed coefficients.");}
            const auto saved=result;
            {ScopedComputeBackend other(ComputeBackend::Gpu);check(!gpuSourceWavelet(frame,planes,tile,map,means)&&result==saved,"Forward reused another job's source.");}
        }
    }
    // Two immutable jobs interleave original/working RGBA at different sizes.
    // Restoring a job must hit its own GPU source, not upload or reuse another
    // frame. New jobs at a recycled address must always upload fresh pixels.
    {
        auto frame=[](uint32_t w,uint32_t h,uint8_t value){Frame f{w,h,0,1,Bytes(pixelBytes(w,h),255)};for(size_t at=0;at<f.pixels.size();at+=4){f.pixels[at]=value;f.pixels[at+1]=value/2;f.pixels[at+2]=255-value;}return f;};
        auto a=frame(512,512,41),aw=frame(512,640,73),b=frame(640,512,109),bw=frame(512,768,151);
        auto stats=[](const Frame& f){std::vector<GpuSourceStatistics> values(size_t((f.width+15)/16)*((f.height+15)/16));check(gpuSourceStatistics(f,16,0,values),"GPU source cache test declined.");return values;};
        auto same=[](const auto& a,const auto& b){return a.size()==b.size()&&!std::memcmp(a.data(),b.data(),a.size()*sizeof(GpuSourceStatistics));};
        std::vector<GpuSourceStatistics> original;
        {ScopedComputeBackend first(ComputeBackend::Gpu);original=stats(a);const auto working=stats(aw);
            {ScopedComputeBackend second(ComputeBackend::Gpu);const auto other=stats(b),otherWorking=stats(bw);const auto bytes=gpuActivity().uploadedBytes;
                check(same(other,stats(b))&&same(otherWorking,stats(bw)),"Interleaved GPU source cache returned another image.");
                check(gpuActivity().uploadedBytes==bytes,"Interleaved GPU sources were uploaded again.");}
            const auto bytes=gpuActivity().uploadedBytes;
            check(same(original,stats(a))&&same(working,stats(aw)),"Restored GPU source job changed statistics.");
            check(gpuActivity().uploadedBytes==bytes,"Restored GPU source job was uploaded again.");
        }
        for(size_t at=0;at<a.pixels.size();at+=4)a.pixels[at]=219;
        {ScopedComputeBackend next(ComputeBackend::Gpu);const auto bytes=gpuActivity().uploadedBytes;const auto changed=stats(a);
            check(!same(original,changed)&&gpuActivity().uploadedBytes==bytes+a.pixels.size(),"GPU source cache reused a recycled frame address.");}
    }
    // Backend scopes are independent across calling threads and restore on exit.
    {ScopedComputeBackend scope(ComputeBackend::Gpu);
        check(std::async(std::launch::async,[]{return computeBackend();}).get()==ComputeBackend::Cpu,"Backend leaked across threads.");}
    check(computeBackend()==ComputeBackend::Cpu,"Backend scope was not restored.");
    const auto a=gpuActivity();check(a.transforms>0&&a.resamples>0&&a.nativeDecodes>0&&a.analyses>0&&a.contourFilters>=8&&a.fallbacks==0,"GPU tests silently fell back to CPU.");
    std::cout<<"PASS R470 GPU: "<<a.transforms<<" transforms, "<<a.resamples<<" resamples, max transform error "<<worst<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
