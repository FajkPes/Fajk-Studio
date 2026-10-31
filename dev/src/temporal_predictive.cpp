#include "temporal_predictive.hpp"
#include "fajk/entropy.hpp"
#include "fajk/hardware.hpp"
#include "fajk/compute.hpp"
#include "wavelet.hpp"
#include "spatial.hpp"
#include "gpu_compute.hpp"
#include "temporal_block_transform.hpp"
#include "timing.hpp"
#include "predictive_color_avx2.hpp"
#include "motion_warp_avx2.hpp"
#include <chrono>
#include <cstring>
#include <bit>
#include <atomic>

namespace fajk {
bool hasAvx2();
bool waveletOpaqueAvx2(const uint8_t*,size_t);
bool predictiveOpaque(const Frame& f){
    if(hasAvx2())return waveletOpaqueAvx2(f.pixels.data(),f.pixels.size());
    for(size_t i=3;i<f.pixels.size();i+=4)if(f.pixels[i]!=255)return false;return true;
}
namespace {
using Clock=std::chrono::steady_clock;
double ms(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
constexpr uint32_t cell=32,maskCell=8;
struct Motion { int16_t x=0,y=0; };
struct Prediction {uint32_t cols=0,rows=0;std::vector<Motion> vectors;std::array<int,3> bias{};bool blocks=false;std::vector<uint32_t> affine;};
int luma(const Frame& f,int x,int y){x=std::clamp(x,0,int(f.width)-1);y=std::clamp(y,0,int(f.height)-1);const auto p=(size_t(y)*f.width+x)*4;return (int(f.pixels[p])+2*int(f.pixels[p+1])+int(f.pixels[p+2])+2)/4;}
int channel(const Frame& f,int x4,int y4,int c){
    const int x=std::clamp(x4,0,int(f.width-1)*4),y=std::clamp(y4,0,int(f.height-1)*4);
    const int ix=x/4,iy=y/4,nx=std::min(ix+1,int(f.width)-1),ny=std::min(iy+1,int(f.height)-1),fx=x%4,fy=y%4;
    auto at=[&](int xx,int yy){return int(f.pixels[(size_t(yy)*f.width+xx)*4+c]);};
    return ((at(ix,iy)*(4-fx)+at(nx,iy)*fx)*(4-fy)+(at(ix,ny)*(4-fx)+at(nx,ny)*fx)*fy+8)/16;
}
Motion flow(const Prediction& p,uint32_t x,uint32_t y){
    if(p.blocks){const auto at=size_t(y/cell)*p.cols+x/cell;auto v=p.vectors[at];if(!p.affine.empty()){
        const auto g=p.affine[at];int values[4];for(unsigned n=0;n<4;++n)values[n]=int(int8_t(uint8_t(g>>(n*8))));
        const int lx=int(x%32)-16,ly=int(y%32)-16;auto delta=[](int value){return value>=0?(value+128)/256:-((-value+128)/256);};
        v.x=int16_t(v.x+delta(values[0]*lx+values[1]*ly));v.y=int16_t(v.y+delta(values[2]*lx+values[3]*ly));}return v;}
    const int gx=int(x)-16,gy=int(y)-16;
    const int lx=gx<0?0:std::min(gx/32,int(p.cols)-1),ly=gy<0?0:std::min(gy/32,int(p.rows)-1);
    const int hx=std::min(lx+1,int(p.cols)-1),hy=std::min(ly+1,int(p.rows)-1),fx=gx<0?0:gx%32,fy=gy<0?0:gy%32;
    auto component=[&](bool vertical){auto at=[&](int xx,int yy){const auto v=p.vectors[size_t(yy)*p.cols+xx];return vertical?int(v.y):int(v.x);};
        const int sum=(at(lx,ly)*(32-fx)+at(hx,ly)*fx)*(32-fy)+(at(lx,hy)*(32-fx)+at(hx,hy)*fx)*fy;
        return int16_t(sum>=0?(sum+512)/1024:-((-sum+512)/1024));};
    return {component(false),component(true)};
}
Frame warp(const Frame& f,const Prediction& p,uint32_t workers,std::span<const uint8_t> modes={},const std::function<void()>& whilePending={}){
    Frame out{f.width,f.height,f.pts,f.duration,Bytes(f.pixels.size())};
    if(computeBackend()==ComputeBackend::Gpu){std::vector<uint32_t> vectors(p.vectors.size());for(size_t i=0;i<vectors.size();++i)vectors[i]=std::bit_cast<uint32_t>(p.vectors[i]);
        if(gpuMotionWarp(f,cell,vectors,p.bias,modes,out.pixels,p.blocks,p.affine,whilePending))return out;}
    const bool avx=p.blocks&&hasAvx2();
    if(avx){std::vector<uint32_t> vectors(p.vectors.size());for(size_t i=0;i<vectors.size();++i)vectors[i]=std::bit_cast<uint32_t>(p.vectors[i]);
        parallelFor(f.height,workers,[&](uint32_t y){const auto start=motionWarpRowAvx2(f.pixels.data(),out.pixels.data()+size_t(y)*f.width*4,f.width,f.height,y,vectors,p.affine,modes,p.bias);
            for(uint32_t x=start;x<f.width;++x){const auto v=!modes.empty()&&(modes[size_t(y/8)*((f.width+7)/8)+x/8]&2)?Motion{}:flow(p,x,y);const auto at=(size_t(y)*f.width+x)*4;
                for(int c=0;c<3;++c)out.pixels[at+c]=uint8_t(std::clamp(channel(f,int(x)*4+v.x,int(y)*4+v.y,c)+p.bias[c],0,255));out.pixels[at+3]=255;}});return out;}
    parallelFor(f.height,workers,[&](uint32_t y){for(uint32_t x=0;x<f.width;++x){const auto v=!modes.empty()&&(modes[size_t(y/8)*((f.width+7)/8)+x/8]&2)?Motion{}:flow(p,x,y);const auto at=(size_t(y)*f.width+x)*4;
        for(int c=0;c<3;++c)out.pixels[at+c]=uint8_t(std::clamp(channel(f,int(x)*4+v.x,int(y)*4+v.y,c)+p.bias[c],0,255));out.pixels[at+3]=255;}});return out;
}
// A skip boundary has only 512 possible 3x3 neighbourhoods and 64 phases.
// Compute the neighbourhood once per block, not nine bounds checks per pixel
// in both the encoder and decoder. The 32 KiB table preserves exact feathering.
struct Blends {uint32_t cols;std::vector<uint16_t> patterns;};
Blends makeBlends(std::span<const uint8_t> keep,uint32_t cols,uint32_t rows){
    Blends result{cols,std::vector<uint16_t>(keep.size())};
    for(uint32_t y=0;y<rows;++y)for(uint32_t x=0;x<cols;++x){auto& pattern=result.patterns[size_t(y)*cols+x];
        if(!(keep[size_t(y)*cols+x]&1)){pattern=16;continue;}
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const int xx=int(x)+dx,yy=int(y)+dy;
            if(xx>=0&&yy>=0&&xx<int(cols)&&yy<int(rows)&&!(keep[size_t(yy)*cols+xx]&1))pattern|=uint16_t(1<<((dy+1)*3+dx+1));}
    }return result;
}
const std::array<std::array<uint8_t,64>,512>& blendTable(){
    static const auto table=[] {std::array<std::array<uint8_t,64>,512> result{};
        for(unsigned pattern=0;pattern<512;++pattern)for(int y=0;y<8;++y)for(int x=0;x<8;++x){unsigned blend=pattern&16?4:0;
            for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)if(pattern&(1<<((dy+1)*3+dx+1))){
                const int distance=std::max({dx*8-x,x-(dx*8+7),dy*8-y,y-(dy*8+7),0});if(distance<4)blend=std::max(blend,unsigned(4-distance));}
            result[pattern][y*8+x]=uint8_t(blend);}
        return result;}();
    return table;
}
unsigned blendAt(const Blends& map,uint32_t x,uint32_t y){
    return blendTable()[map.patterns[size_t(y/8)*map.cols+x/8]][(y%8)*8+x%8];
}
void fitIllumination(const Frame& current,const Frame& source,Prediction& prediction){
    // Fit against the original witness, not its quantization error. Most of
    // the image must support a common shift; moving objects and cuts must not
    // induce a global color change. Existing packet/warp syntax carries bias.
    std::array<std::array<uint32_t,65>,3> histograms{};
    std::vector<std::array<int16_t,6>> samples;
    for(uint32_t y=8;y<current.height;y+=16)for(uint32_t x=8;x<current.width;x+=16){
        const auto offset=flow(prediction,x,y);std::array<int16_t,6> values{};
        for(unsigned c=0;c<3;++c){const int a=current.pixels[(size_t(y)*current.width+x)*4+c],b=channel(source,int(x)*4+offset.x,int(y)*4+offset.y,int(c));
            values[c]=int16_t(a);values[c+3]=int16_t(b);++histograms[c][std::clamp(a-b,-32,32)+32];}
        samples.push_back(values);
    }
    if(samples.empty())return;
    for(unsigned c=0;c<3;++c){uint32_t cumulative=0;int bias=0;
        for(unsigned i=0;i<65;++i){cumulative+=histograms[c][i];if(cumulative>samples.size()/2){bias=int(i)-32;break;}}
        if(!bias)continue;
        uint64_t before=0,after=0;size_t supported=0;
        for(const auto& s:samples){const int d=s[c]-s[c+3],corrected=s[c]-std::clamp(s[c+3]+bias,0,255);
            before+=d*d;after+=corrected*corrected;supported+=std::abs(d-bias)<=1;}
        if(supported*10>=samples.size()*9&&after*4<=before)prediction.bias[c]=bias;
    }
}
Prediction motion(const Frame& f,const Frame& key,uint32_t workers,uint32_t tolerance,bool gpuBlocks=false,bool affine=false,const Frame* illuminationReference=nullptr){
    Prediction p;p.cols=(f.width+31)/32;p.rows=(f.height+31)/32;p.vectors.resize(size_t(p.cols)*p.rows);
    p.blocks=gpuBlocks;
    if(gpuBlocks){std::vector<uint32_t> vectors(p.vectors.size());if(affine)p.affine.resize(vectors.size());if(gpuBlockMotion(f,key,vectors,p.affine)){
        for(size_t i=0;i<vectors.size();++i)p.vectors[i]=std::bit_cast<Motion>(vectors[i]);
        if(illuminationReference)fitIllumination(f,*illuminationReference,p);return p;}}
    auto globalCost=[&](int dx,int dy){int64_t sum=0,square=0;uint32_t n=0;for(uint32_t y=16;y<f.height;y+=32)for(uint32_t x=16;x<f.width;x+=32){const int d=luma(f,x,y)-luma(key,int(x)+dx,int(y)+dy);sum+=d;square+=int64_t(d)*d;++n;}return n?square-sum*sum/n:0;};
    int gx=0,gy=0;auto best=globalCost(0,0);
    for(int step:{32,16,8,4,2,1}){const int ox=gx,oy=gy;for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const int xx=ox+dx*step,yy=oy+dy*step;const auto cost=globalCost(xx,yy);if(cost<best){best=cost;gx=xx;gy=yy;}}}
    if(affine){
        // Match the GPU policy: a local object/color change must not tint the
        // entire reference. The off switch must also disable fitting on CPU.
        if(illuminationReference){std::fill(p.vectors.begin(),p.vectors.end(),Motion{int16_t(gx*4),int16_t(gy*4)});fitIllumination(f,*illuminationReference,p);}
    }else for(int c=0;c<3;++c){std::vector<int> differences;for(uint32_t y=16;y<f.height;y+=32)for(uint32_t x=16;x<f.width;x+=32)differences.push_back(int(f.pixels[(size_t(y)*f.width+x)*4+c])-channel(key,(int(x)+gx)*4,(int(y)+gy)*4,c));
        if(!differences.empty()){auto mid=differences.begin()+differences.size()/2;std::nth_element(differences.begin(),mid,differences.end());p.bias[c]=std::clamp(*mid,-32,32);}}
    parallelFor(p.rows,workers,[&](uint32_t ty){for(uint32_t tx=0;tx<p.cols;++tx){const int x=int(tx*32),y=int(ty*32);
        uint64_t stationary=0;unsigned stationaryCount=0,stationaryMax=0;
        for(int yy=4;yy<32&&y+yy<int(f.height);yy+=8)for(int xx=4;xx<32&&x+xx<int(f.width);xx+=8)for(int c=0;c<3;++c){
            const auto at=(size_t(y+yy)*f.width+x+xx)*4+c;const auto d=unsigned(std::abs(int(f.pixels[at])-std::clamp(int(key.pixels[at])+p.bias[c],0,255)));stationary+=d*d;stationaryMax=std::max(stationaryMax,d);++stationaryCount;}
        if(stationary<=uint64_t(stationaryCount)*tolerance*tolerance&&stationaryMax<=4*tolerance){p.vectors[size_t(ty)*p.cols+tx]={0,0};continue;}
        auto cost=[&](int dx4,int dy4){int64_t sum=0,square=0;uint32_t n=0;
            for(int yy=4;yy<32&&y+yy<int(f.height);yy+=8)for(int xx=4;xx<32&&x+xx<int(f.width);xx+=8){const int a=luma(f,x+xx,y+yy),b=(channel(key,(x+xx)*4+dx4,(y+yy)*4+dy4,0)+2*channel(key,(x+xx)*4+dx4,(y+yy)*4+dy4,1)+channel(key,(x+xx)*4+dx4,(y+yy)*4+dy4,2)+2)/4;
                const int d=a-b;sum+=d;square+=int64_t(d)*d;++n;}
            const int bias=(p.bias[0]+2*p.bias[1]+p.bias[2])/4;
            return square-2*bias*sum+int64_t(n)*bias*bias+(int64_t(std::abs(dx4-gx*4)+std::abs(dy4-gy*4)))*2;};
        int dx=gx*4,dy=gy*4;auto score=cost(dx,dy);
        for(int step:{64,32,16,8,4,2,1}){const int ox=dx,oy=dy;for(int yy=-1;yy<=1;++yy)for(int xx=-1;xx<=1;++xx){const int nx=ox+xx*step,ny=oy+yy*step;const auto candidate=cost(nx,ny);if(candidate<score){score=candidate;dx=nx;dy=ny;}}}
        p.vectors[size_t(ty)*p.cols+tx]={int16_t(dx),int16_t(dy)};
    }});return p;
}
void transform(std::array<std::vector<float>,3>& planes,uint32_t w,uint32_t h,uint32_t workers,bool inverse,ComputeBackend backend){
    ScopedComputeBackend compute(backend);std::array<GpuPlane,3> gpu;for(int c=0;c<3;++c)gpu[c]={planes[c],c?w/2:w,c?h/2:h};
    if(backend==ComputeBackend::Gpu&&gpuWavelet(gpu,inverse))return;
    parallelFor(3,workers,[&](uint32_t c){waveletTransform(planes[c],int(c?w/2:w),int(c?h/2:h),0,inverse);});
}
struct Band {uint32_t x,y,width,height;float step;};
Band residualBand(uint32_t width,uint32_t height,uint32_t index,uint32_t step,uint32_t curve=65){
    if(index==18)return {0,0,width>>6,height>>6,std::max(.25f,step*(curve==65?.1f:std::pow(curve*.01f,6.f)))};
    const uint32_t level=index/3,orientation=index%3,bw=width>>(level+1),bh=height>>(level+1);
    return {orientation==1?0:bw,orientation==0?0:bh,bw,bh,
        std::max(.35f,float(step)*(orientation==2?1.35f:1.f)*std::pow(curve*.01f,float(level)))};
}
void inverseResidual(std::array<std::vector<float>,3>& planes,uint32_t w,uint32_t h,uint32_t workers,bool blockTransform){
    TimingTimeline timing;
    if(blockTransform){for(unsigned c=0;c<3;++c)temporalBlockTransform(planes[c],c?w/2:w,c?h/2:h,workers,true);}
    else transform(planes,w,h,workers,true,ComputeBackend::Cpu);
    timing.mark("decode.motion.inverse");
}
Frame reconstructResidual(Frame result,std::array<std::vector<float>,3>& planes,uint32_t w,uint32_t h,uint32_t workers,std::span<const uint8_t> keep,bool blockTransform,bool inverseDone=false){
    // Every pixel is a prediction when all blocks are exact skips. Entropy
    // validation still happens in the caller, but synthesis cannot affect it.
    if(std::all_of(keep.begin(),keep.end(),[](uint8_t v){return (v&1)!=0;}))return result;
    const auto width=result.width,height=result.height,cols=(width+7)/8,rows=(height+7)/8;
    if(!inverseDone)inverseResidual(planes,w,h,workers,blockTransform);
    TimingTimeline timing;
    const auto blends=makeBlends(keep,cols,rows);
    struct Tap {uint32_t low,high;float fraction;};
    const bool avx=hasAvx2();const uint32_t scalarStart=avx?width/8*8:0;
    std::vector<Tap> taps(width-scalarStart);
    for(uint32_t x=scalarStart;x<width;++x){const float sx=std::max(0.f,(x+.5f)/2-.5f);const auto low=uint32_t(sx);taps[x-scalarStart]={low,std::min(low+1,w/2-1),sx-low};}
    parallelFor(height,workers,[&](uint32_t y){
        const float sy=std::max(0.f,(y+.5f)/2-.5f);const auto low=uint32_t(sy),high=std::min(low+1,h/2-1);const float fy=sy-low;
        std::array<const float*,2> top{planes[1].data()+size_t(low)*(w/2),planes[2].data()+size_t(low)*(w/2)};
        std::array<const float*,2> bottom{planes[1].data()+size_t(high)*(w/2),planes[2].data()+size_t(high)*(w/2)};
        uint32_t start=0;
        if(avx)start=predictiveColorRowAvx2(result.pixels.data()+size_t(y)*width*4,width,w/2,planes[0].data()+size_t(y)*w,top[0],bottom[0],top[1],bottom[1],blends.patterns.data()+size_t(y/8)*cols,blendTable().data(),y%8,fy);
        for(uint32_t x=start;x<width;++x){
        const unsigned blend=blendAt(blends,x,y);if(!blend)continue;
        const auto tap=taps[x-scalarStart];const auto lx=tap.low,hx=tap.high;const float fx=tap.fraction;
        auto value=[&](int c){return (top[c][lx]*(1-fx)+top[c][hx]*fx)*(1-fy)+(bottom[c][lx]*(1-fx)+bottom[c][hx]*fx)*fy;};
        // Luma is already at the output resolution. Only the two chroma
        // planes need interpolation; evaluating luma's four taps wasted work.
        const auto at=(size_t(y)*width+x)*4;const float yy=planes[0][size_t(y)*w+x],co=value(0),cg=value(1);const float delta[3]={yy+co-cg,yy+cg,yy-co-cg};
        for(int c=0;c<3;++c){const float correction=std::clamp(delta[c]*blend*.25f,-255.f,255.f);
            // Double addition preserves lround's half-away rule even for a
            // float immediately below .5, without three library calls/pixel.
            const int rounded=int(double(correction)+(correction<0?-.5:.5));
            result.pixels[at+c]=uint8_t(std::clamp(int(result.pixels[at+c])+rounded,0,255));}
    }});timing.mark("decode.motion.color");return result;
}
void filterReference(Frame& frame,uint32_t step,uint32_t workers,std::span<const uint8_t> keep,bool adjacent=false){
    // Spatial only: never blend a previous frame into the current picture.
    // Luma guides a stronger chroma average. RGB-distance gating would reject
    // the very colored speckles we need to remove. Large real color edges
    // still receive a range penalty; luminance is smoothed much more gently.
    // Immutable input avoids scan-order dependence between rows/threads.
    // Near-lossless residuals need no spatial smoothing. A fixed minimum
    // strength would soften real low-contrast detail even at a unit step.
    if(step<4||std::all_of(keep.begin(),keep.end(),[](uint8_t v){return (v&1)!=0;}))return;
    if(gpuReferenceFilter(frame,step,keep,adjacent))return;
    const auto source=frame.pixels;const unsigned range=std::clamp(step/2,2u,24u),lumaRange=std::min(step,8u);
    parallelFor(frame.height,workers,[&](uint32_t y){for(uint32_t x=0;x<frame.width;++x){
        if(keep[size_t(y/8)*((frame.width+7)/8)+x/8]&1)continue;
        const auto at=(size_t(y)*frame.width+x)*4;
        const int r=source[at],g=source[at+1],b=source[at+2],yy=r+2*g+b,co=r-b,cg=2*g-r-b;
        int weight=int(range)*4,coSum=co*weight,cgSum=cg*weight,yWeight=64,ySum=yy*yWeight;
        for(int dy=-2;dy<=2;dy+=2)for(int dx=-2;dx<=2;dx+=2){if(!dx&&!dy)continue;
            const auto nx=uint32_t(std::clamp(int(x)+(adjacent?dx/2:dx),0,int(frame.width)-1)),ny=uint32_t(std::clamp(int(y)+(adjacent?dy/2:dy),0,int(frame.height)-1));
            const auto n=(size_t(ny)*frame.width+nx)*4;const int nr=source[n],ng=source[n+1],nb=source[n+2];
            const int nY=nr+2*ng+nb,nCo=nr-nb,nCg=2*ng-nr-nb;
            const unsigned luma=unsigned(std::abs(yy-nY))/4;
            const unsigned difference=std::max({luma,unsigned(std::abs(co-nCo))/8,unsigned(std::abs(cg-nCg))/16});
            const int w=int(range-std::min(range,difference));weight+=w;coSum+=nCo*w;cgSum+=nCg*w;
            const int wy=int(lumaRange-std::min(lumaRange,luma));yWeight+=wy;ySum+=nY*wy;
        }
        auto divide=[](int value,int divisor){return value<0?-((-value+divisor/2)/divisor):(value+divisor/2)/divisor;};
        const int yOut=divide(ySum,yWeight),coOut=divide(coSum,weight),cgOut=divide(cgSum,weight);
        frame.pixels[at]=uint8_t(std::clamp(divide(yOut+2*coOut-cgOut,4),0,255));
        frame.pixels[at+1]=uint8_t(std::clamp(divide(yOut+cgOut,4),0,255));
        frame.pixels[at+2]=uint8_t(std::clamp(divide(yOut-2*coOut-cgOut,4),0,255));
    }});
}

}
PredictiveTrial trialPredictive(const Frame& f,const Frame& sourceKey,const Frame& decodedKey,uint32_t workers,uint32_t tolerance,uint32_t step,ComputeBackend backend,std::span<const uint8_t> ages,uint32_t hold,uint32_t propagatedError,bool compact,uint32_t curve,const Frame* codedTarget,bool blockTransform,bool gpuBlocks,bool affine,size_t maximumBytes,bool residentPreparation,bool illuminationPrediction,bool compactMotion,uint32_t maximumStep,bool inLoopFilter,bool adjacentFilter){
    ScopedComputeBackend compute(backend);
    f.validate();sourceKey.validate();decodedKey.validate();
    require(decodedKey.width==f.width&&decodedKey.height==f.height&&tolerance<=16,"Invalid predictive reference.");
    if(codedTarget){codedTarget->validate();require(codedTarget->width==f.width&&codedTarget->height==f.height,"Invalid coded target.");}
    const auto& residualTarget=codedTarget?*codedTarget:f;
    require(curve>=50&&curve<=100,"Invalid residual quantizer curve.");
    require(propagatedError<=32,"Invalid propagated error limit.");
    require((!affine||gpuBlocks)&&(!gpuBlocks||(!blockTransform&&!compact&&curve==65)),"Unsupported block motion trial combination.");
    require(f.width==sourceKey.width&&f.height==sourceKey.height&&decodedKey.pixels.size()==f.pixels.size()&&step>=1&&step<=256,"Invalid predictive trial.");
    require(!maximumStep||(affine&&maximumStep>=step&&maximumStep<=256),"Invalid predictive rate range.");
    require(!inLoopFilter||affine,"Reference filtering requires affine residuals.");
    require(!adjacentFilter||inLoopFilter,"Adjacent reference filtering requires the in-loop filter.");
    require(predictiveOpaque(f)&&predictiveOpaque(sourceKey),"Predictive trial requires opaque frames.");
    PredictiveTrial trial;auto start=Clock::now();auto prediction=motion(f,sourceKey,workers,tolerance,gpuBlocks,affine,affine&&illuminationPrediction?&sourceKey:nullptr);
    trial.motionMs=ms(start);start=Clock::now();
    const uint32_t cols=(f.width+7)/8,rows=(f.height+7)/8,w=(f.width+127)&~127u,h=(f.height+127)&~127u;
    require(hold>=1&&hold<=120&&(ages.empty()||ages.size()==size_t(cols)*rows),"Invalid age map.");
    Bytes keep(size_t(cols)*rows);trial.ages.resize(keep.size());std::array<std::vector<float>,3> planes;for(int c=0;c<3;++c)planes[c].resize(size_t(c?w/2:w)*(c?h/2:h));
    std::array<GpuPlane,3> gpuPlanes;for(unsigned c=0;c<3;++c)gpuPlanes[c]={planes[c],c?w/2:w,c?h/2:h};
    Frame predicted,witness;
    bool prepared=false;
    if(residentPreparation&&affine&&!codedTarget&&backend==ComputeBackend::Gpu&&uint64_t(f.width)*f.height>=512*512){
        predicted={f.width,f.height,f.pts,f.duration,Bytes(f.pixels.size())};
        std::vector<uint32_t> vectors(prediction.vectors.size());for(size_t i=0;i<vectors.size();++i)vectors[i]=std::bit_cast<uint32_t>(prediction.vectors[i]);
        prepared=gpuPreparePredictive(f,sourceKey,decodedKey,vectors,prediction.affine,prediction.bias,tolerance,propagatedError,predicted.pixels,keep,gpuPlanes,inLoopFilter);
    }
    if(!prepared){
    predicted=warp(decodedKey,prediction,workers);witness=warp(sourceKey,prediction,workers);
    parallelFor(rows,workers,[&](uint32_t ty){for(uint32_t tx=0;tx<cols;++tx){uint64_t squared=0,stationarySquared=0,codedSquared=0,stationaryCodedSquared=0;unsigned maximum=0,stationaryMaximum=0,count=0;
        for(uint32_t y=ty*8;y<std::min(f.height,ty*8+8);++y)for(uint32_t x=tx*8;x<std::min(f.width,tx*8+8);++x)for(size_t c=0;c<3;++c){const auto at=(size_t(y)*f.width+x)*4+c;
            const int current=f.pixels[at];const auto d=unsigned(std::abs(current-int(witness.pixels[at]))),sd=unsigned(std::abs(current-std::clamp(int(sourceKey.pixels[at])+prediction.bias[c],0,255)));
            squared+=d*d;maximum=std::max(maximum,d);stationarySquared+=sd*sd;stationaryMaximum=std::max(stationaryMaximum,sd);++count;
            if(affine){const int cd=current-int(predicted.pixels[at]),scd=current-std::clamp(int(decodedKey.pixels[at])+prediction.bias[c],0,255);codedSquared+=cd*cd;stationaryCodedSquared+=scd*scd;}}
        // Choose the actual reconstructed predictor that the decoder will
        // see. Source-only matching can move reference quantization noise in
        // an almost stationary sky/HUD even when zero motion codes better.
        const bool stationary=affine?(stationaryCodedSquared<=codedSquared||(stationaryMaximum<=tolerance&&stationaryCodedSquared<=codedSquared+uint64_t(count)*tolerance*tolerance/4)):stationarySquared<=squared;
        if(stationary){squared=stationarySquared;maximum=stationaryMaximum;
            for(uint32_t y=ty*8;y<std::min(f.height,ty*8+8);++y)for(uint32_t x=tx*8;x<std::min(f.width,tx*8+8);++x)for(int c=0;c<3;++c){const auto at=(size_t(y)*f.width+x)*4+c;predicted.pixels[at]=uint8_t(std::clamp(int(decodedKey.pixels[at])+prediction.bias[c],0,255));witness.pixels[at]=uint8_t(std::clamp(int(sourceKey.pixels[at])+prediction.bias[c],0,255));}}
        const auto v=stationary?Motion{}:flow(prediction,std::min(f.width-1,tx*8+4),std::min(f.height-1,ty*8+4));
        const int ax=std::clamp((int(tx*8+4)*4+v.x)/32,0,int(cols)-1),ay=std::clamp((int(ty*8+4)*4+v.y)/32,0,int(rows)-1);
        const unsigned previousAge=ages.empty()?(tx*13+ty*7)%hold:ages[size_t(ay)*cols+ax];
        // Approximate hard skip masks in the wavelet residual produce visible
        // 8x8 steps when neighbouring blocks retain different reference errors.
        // Max compression uses the continuous bounded-error residual below;
        // exact source matches may still skip. Other diagnostic modes retain
        // their original approximate masks and age-based refresh semantics.
        const bool retained=squared<=uint64_t(count)*tolerance*tolerance&&maximum<=tolerance*(affine?1u:4u)&&(!maximum||previousAge+1<hold)&&(!affine||!maximum);keep[size_t(ty)*cols+tx]=uint8_t((retained?1:0)|(stationary?2:0));
        trial.ages[size_t(ty)*cols+tx]=uint8_t(retained&&maximum?previousAge+1:0);
    }});
    const bool fused=affine&&!codedTarget&&gpuPredictiveWavelet(f,predicted,witness,keep,propagatedError,gpuPlanes,inLoopFilter);
    if(!fused){
    const auto blends=makeBlends(keep,cols,rows);
    parallelFor((f.height+1)/2,workers,[&](uint32_t cy){for(uint32_t y=cy*2;y<std::min(f.height,cy*2+2);++y)for(uint32_t x=0;x<f.width;++x){const auto at=(size_t(y)*f.width+x)*4,p=size_t(y)*w+x;
            const auto blend=blendAt(blends,x,y);
            // Preserve a bounded amount of the reference's existing coding
            // error. Otherwise every small source change re-encodes texture
            // discarded by the key frame, wasting bits and changing its noise.
            // The excess is corrected, so this is not an open-loop source delta.
            const int lumaError=(int(predicted.pixels[at])-int(witness.pixels[at])+2*(int(predicted.pixels[at+1])-int(witness.pixels[at+1]))+int(predicted.pixels[at+2])-int(witness.pixels[at+2]))/4;
            auto residual=[&](int c){if(!blend)return 0.f;const int delta=int(residualTarget.pixels[at+c])-int(predicted.pixels[at+c]);
                // Full error carry is an integrator: new quantization noise
                // accumulates throughout a prediction chain. Retain only half
                // the bounded luma error; exact-source skips remain untouched.
                if(inLoopFilter)return float(delta+std::clamp(lumaError,-int(propagatedError),int(propagatedError))/2);
                return float(delta+std::clamp(int(predicted.pixels[at+c])-int(witness.pixels[at+c]),-int(propagatedError),int(propagatedError)));};
            const float r=residual(0),g=residual(1),b=residual(2);
            planes[0][p]=(r+2*g+b)*.25f;
            // Each work item owns both rows of complete chroma cells.
            // Normalize partial cells by their actual area at odd borders.
            const auto cp=size_t(y/2)*(w/2)+x/2;const float area=float(std::min(2u,f.width-(x/2)*2)*std::min(2u,f.height-(y/2)*2));planes[1][cp]+=(r-b)*(.5f/area);planes[2][cp]+=(2*g-r-b)*(.25f/area);
            // The witness follows the same pre-quantization blend as the
            // reconstruction. This prevents small accepted changes and
            // residual feathering errors from silently accumulating.
            if(!affine)for(int c=0;c<3;++c)witness.pixels[at+c]=uint8_t((unsigned(witness.pixels[at+c])*(4-blend)+unsigned(f.pixels[at+c])*blend+2)/4);
        }
    });
    for(int c=0;c<3;++c){const auto stride=c?w/2:w,ph=c?h/2:h,vw=c?(f.width+1)/2:f.width,vh=c?(f.height+1)/2:f.height;
        parallelFor(ph,workers,[&](uint32_t y){auto* target=planes[c].data()+size_t(y)*stride;const auto* source=planes[c].data()+size_t(std::min(y,vh-1))*stride;if(y>=vh)std::copy_n(source,vw,target);std::fill(target+vw,target+stride,source[vw-1]);});}
    if(blockTransform){for(unsigned c=0;c<3;++c)temporalBlockTransform(planes[c],c?w/2:w,c?h/2:h,workers,false);}
    else transform(planes,w,h,workers,false,backend);
    }
    }
    // In affine mode only exact-source blocks retain a skip mask. Their
    // witness equals the source already; every other pixel has blend 4.
    trial.transformMs=ms(start);start=Clock::now();
    ByteWriter out;out.u32(0x334b4a46);out.u8(134);out.u8(1);out.u16(adjacentFilter?16:inLoopFilter?15:affine?14:gpuBlocks?13:blockTransform?12:((curve==65?8:10)+(compact?1:0)));out.u32(f.width);out.u32(f.height);out.u32(step);if(curve!=65&&!blockTransform)out.u32(curve);
    for(auto b:prediction.bias)out.signedVar(b);
    auto encodeVectors=[](std::span<const Motion> field){ByteWriter bytes;for(auto v:field){bytes.signedVar(v.x);bytes.signedVar(v.y);}return entropyEncode(bytes.data);};
    auto encodeGradients=[](std::span<const uint32_t> field){ByteWriter bytes;for(auto g:field)bytes.u32(g);return entropyEncode(bytes.data);};
    auto codedVectors=encodeVectors(prediction.vectors);Bytes codedGradients;if(affine)codedGradients=encodeGradients(prediction.affine);
    const auto originalMotionBytes=codedVectors.size()+codedGradients.size();
    if(compactMotion&&affine){
        auto vectors=prediction.vectors;auto gradients=prediction.affine;bool changed=false;
        for(uint32_t by=0;by<prediction.rows;++by)for(uint32_t bx=0;bx<prediction.cols;++bx){bool used=false;
            for(uint32_t y=by*4;y<std::min(rows,by*4+4)&&!used;++y)for(uint32_t x=bx*4;x<std::min(cols,bx*4+4);++x)if(!(keep[size_t(y)*cols+x]&2)){used=true;break;}
            const auto at=size_t(by)*prediction.cols+bx;
            if(!used&&(vectors[at].x||vectors[at].y||gradients[at])){vectors[at]={};gradients[at]=0;changed=true;}
        }
        // A stationary 8x8 mode overrides both vector and affine gradient.
        // Normalize wholly unused 32x32 entries; keep each original stream if
        // its entropy model happens to encode the normalized data less well.
        if(changed){auto smaller=encodeVectors(vectors);if(smaller.size()<codedVectors.size())codedVectors=std::move(smaller);
            smaller=encodeGradients(gradients);if(smaller.size()<codedGradients.size())codedGradients=std::move(smaller);}
    }
    out.u32(uint32_t(codedVectors.size()));out.bytes(codedVectors);
    if(affine){out.u32(uint32_t(codedGradients.size()));out.bytes(codedGradients);}
    const auto codedKeep=entropyEncode(keep);out.u32(uint32_t(codedKeep.size()));out.bytes(codedKeep);
    const auto bandCount=blockTransform?192u:57u;std::vector<Bytes> coded(bandCount);
    // A completed band's bytes can never shrink as other independent bands
    // finish. Stop scheduling entropy work once even this lower bound cannot
    // fit. Accepted candidates retain exactly the same bands and byte stream.
    const auto motionSaving=originalMotionBytes-codedVectors.size()-codedGradients.size();
    const bool adaptiveRate=maximumStep>step;
    if(adaptiveRate){uint64_t error=0,samples=0;
        for(uint32_t y=std::min(4u,f.height-1);y<f.height;y+=8)for(uint32_t x=std::min(4u,f.width-1);x<f.width;x+=8)for(unsigned c=0;c<3;++c){
            const auto at=(size_t(y)*f.width+x)*4+c;const int delta=int(f.pixels[at])-int(predicted.pixels[at]);error+=uint64_t(delta*delta);++samples;
        }
        trial.predictionMse=double(error)/double(samples);
    }
    std::vector<std::vector<float>> selected(adaptiveRate?bandCount:0);
    uint32_t chosenStep=step;
    for(;;){
    std::atomic<size_t> codedBytes{out.data.size()+motionSaving+size_t(bandCount)*4};
    parallelFor(bandCount,workers,[&](uint32_t index){if(codedBytes.load(std::memory_order_relaxed)>maximumBytes)return;
        const auto c=index/(blockTransform?64:19),cw=c?w/2:w,ch=c?h/2:h;auto b=blockTransform?Band{0,0,cw/8,ch/8,float(chosenStep)}:residualBand(cw,ch,index%19,chosenStep,curve);if(affine&&c)b.step*=.5f;const auto* source=planes[c].data()+(blockTransform?size_t(index%64)*b.width*b.height:size_t(b.y)*cw+b.x);std::vector<float> localIntegers;auto& integers=adaptiveRate?selected[index]:localIntegers;
        uint32_t inputStride=blockTransform?b.width:cw;
        // Firm shrinkage is continuous at both thresholds. Hard thresholding
        // broad chroma bands deleted real color structure and made small
        // source changes switch coefficients abruptly on/off. Restrict this
        // cleanup to the finest scales and retain strong edges unchanged.
        const auto band=index%19,level=band/3,orientation=band%3;
        thread_local std::vector<float> clean;
        if(inLoopFilter&&band<18&&((c&&level<2)||(!c&&level==0&&orientation==2))){
            clean.resize(size_t(b.width)*b.height);const float threshold=b.step*(c?1.25f:.75f);
            for(uint32_t y=0;y<b.height;++y)for(uint32_t x=0;x<b.width;++x){const float value=source[size_t(y)*cw+x];
                const float magnitude=std::abs(value);
                clean[size_t(y)*b.width+x]=std::copysign(std::clamp(2.f*(magnitude-threshold),0.f,magnitude),value);}
            source=clean.data();inputStride=b.width;
        }
        coded[index]=encodeSpatialBand(source,inputStride,b.width,b.height,b.step,false,affine?.25f:.5f,false,0,false,0,true,true,.02f,nullptr,0,nullptr,0,23,0,0,0,nullptr,0,false,90,{},0,{},nullptr,&integers);
        if(compact&&!blockTransform)coded[index]=compactSpatialBand(coded[index],b.width,b.height,false,nullptr,integers);
        codedBytes.fetch_add(coded[index].size(),std::memory_order_relaxed);
        // Reuse the integers selected by the entropy encoder. Decoding the
        // newly written stream and warping the same RGB reference a third time
        // were redundant work in every accepted P frame.
        // Keep the original transform intact for a bounded rate retry. No
        // repeated motion search, GPU warp, transfer or forward transform.
        if(!adaptiveRate){auto* quantized=planes[c].data()+(blockTransform?size_t(index%64)*b.width*b.height:size_t(b.y)*cw+b.x);
            const auto stride=blockTransform?b.width:cw;
            for(uint32_t y=0;y<b.height;++y)for(uint32_t x=0;x<b.width;++x)quantized[size_t(y)*stride+x]=integers[size_t(y)*b.width+x]*b.step;}});
    trial.decisionBytes=codedBytes.load(std::memory_order_relaxed);
    if(trial.decisionBytes<=maximumBytes)break;
    // A scene cut or broadly failed motion fit should refresh the key, rather
    // than fit the budget by increasing distortion throughout a new picture.
    if(!adaptiveRate||chosenStep==maximumStep||trial.predictionMse>1024){trial.overBudget=true;trial.selectedStep=chosenStep;trial.codingMs=ms(start);return trial;}
    chosenStep=std::min(maximumStep,chosenStep*2);++trial.rateRetries;
    for(auto& band:coded)band.clear();
    }
    trial.selectedStep=chosenStep;
    // The chosen step is already part of variants 14/15's packet syntax.
    // Ordinary/older decoders need no additional rate-control metadata.
    for(unsigned b=0;b<4;++b)out.data[16+b]=uint8_t(chosenStep>>(b*8));
    if(adaptiveRate)parallelFor(bandCount,workers,[&](uint32_t index){
        const auto c=index/19,cw=c?w/2:w,ch=c?h/2:h;auto band=residualBand(cw,ch,index%19,chosenStep,curve);if(c)band.step*=.5f;
        auto* quantized=planes[c].data()+size_t(band.y)*cw+band.x;const auto& integers=selected[index];
        for(uint32_t y=0;y<band.height;++y)for(uint32_t x=0;x<band.width;++x)quantized[size_t(y)*cw+x]=integers[size_t(y)*band.width+x]*band.step;
    });
    for(auto& plane:coded){out.u32(uint32_t(plane.size()));out.bytes(plane);}
    trial.packet={std::move(out.data),f.pts,f.duration};trial.codingMs=ms(start);trial.blocks=uint32_t(keep.size());trial.retained=uint32_t(std::count_if(keep.begin(),keep.end(),[](uint8_t v){return v&1;}));
    // A rejected candidate never becomes a prediction reference, so it needs
    // neither inverse transform nor RGB reconstruction.
    if(trial.packet.data.size()<=maximumBytes)trial.image=reconstructResidual(std::move(predicted),planes,w,h,workers,keep,blockTransform);
    if(inLoopFilter&&!trial.image.pixels.empty())filterReference(trial.image,chosenStep,workers,keep,adjacentFilter);
    if(affine)trial.witness=f;else trial.witness=std::move(witness);return trial;
}
PredictiveTrial trialMotion(const Frame& source,const Frame& witness,const Frame& reference,uint32_t workers,const MotionResidualSettings& settings,std::span<const uint8_t> ages){
    require(settings.filter==MotionReferenceFilter::None||settings.filter==MotionReferenceFilter::Legacy||settings.filter==MotionReferenceFilter::Adjacent,"Invalid reference filter.");
    return trialPredictive(source,witness,reference,workers,settings.tolerance,settings.step,settings.backend,ages,settings.holdFrames,settings.errorCarry,
        false,65,nullptr,false,true,true,settings.maximumBytes,settings.residentPreparation,settings.illuminationPrediction,true,settings.maximumStep,
        settings.filter!=MotionReferenceFilter::None,settings.filter==MotionReferenceFilter::Adjacent);
}
Frame decodePredictiveTrial(PacketView packet,const Frame& key,uint32_t workers){
    TimingTimeline timing;
    ByteReader in{packet.data};require(in.u32()==0x334b4a46&&in.u8()==134&&in.u8()==1,"Invalid predictive packet.");
    const auto version=in.u16();require(version>=8&&version<=16,"Invalid predictive version.");
    const auto width=in.u32(),height=in.u32(),step=in.u32();require(width==key.width&&height==key.height&&step>=1&&step<=256,"Invalid predictive dimensions/step.");
    const auto curve=version>=10&&version<=11?in.u32():65u;require(curve>=50&&curve<=100,"Invalid residual quantizer curve.");
    Prediction p;p.blocks=version>=13;p.cols=(width+31)/32;p.rows=(height+31)/32;for(auto& b:p.bias){b=in.signedVar();require(std::abs(int64_t(b))<=32,"Invalid prediction bias.");}
    const auto vbytes=entropyDecode(in.bytes(in.u32()),size_t(p.cols)*p.rows*6);ByteReader vectors{vbytes};p.vectors.resize(size_t(p.cols)*p.rows);
    for(auto& v:p.vectors){const auto x=vectors.signedVar(),y=vectors.signedVar();require(std::abs(int64_t(x))<=512&&std::abs(int64_t(y))<=512,"Invalid motion vector.");v={int16_t(x),int16_t(y)};}vectors.end();
    if(version>=14){const auto coded=entropyDecode(in.bytes(in.u32()),size_t(p.cols)*p.rows*4);require(coded.size()==size_t(p.cols)*p.rows*4,"Invalid affine motion field.");ByteReader gradients{coded};p.affine.resize(p.vectors.size());for(auto& g:p.affine)g=gradients.u32();gradients.end();}
    const uint32_t cols=(width+7)/8,rows=(height+7)/8,w=(width+127)&~127u,h=(height+127)&~127u;
    const auto keep=entropyDecode(in.bytes(in.u32()),size_t(cols)*rows);require(keep.size()==size_t(cols)*rows&&std::all_of(keep.begin(),keep.end(),[](uint8_t v){return v<=3;}),"Invalid keep map.");
    const bool blockTransform=version==12;const auto bandCount=blockTransform?192u:57u;std::vector<std::span<const uint8_t>> coded(bandCount);for(auto& c:coded)c=in.bytes(in.u32());in.end();
    std::array<std::vector<float>,3> planes;for(int c=0;c<3;++c)planes[c].resize(size_t(c?w/2:w)*(c?h/2:h));
    parallelFor(bandCount,workers,[&](uint32_t index){const auto c=index/(blockTransform?64:19),cw=c?w/2:w,ch=c?h/2:h;auto b=blockTransform?Band{0,0,cw/8,ch/8,float(step)}:residualBand(cw,ch,index%19,step,curve);if(version>=14&&c)b.step*=.5f;auto* target=planes[c].data()+(blockTransform?size_t(index%64)*b.width*b.height:size_t(b.y)*cw+b.x);decodeSpatialBand(coded[index],target,blockTransform?b.width:cw,b.width,b.height,b.step,false,false,false,false,true,true,nullptr,0,(version==9||version==11)?31:23);});
    timing.mark("decode.motion.entropy");
    bool inverseDone=false;
    const bool exact=std::all_of(keep.begin(),keep.end(),[](uint8_t v){return (v&1)!=0;});
    auto inverse=[&]{if(!exact&&!inverseDone){inverseResidual(planes,w,h,workers,blockTransform);inverseDone=true;}};
    if(version>=14&&!exact&&computeBackend()==ComputeBackend::Gpu&&uint64_t(width)*height>=512*512){
        Frame accelerated{width,height,packet.pts,packet.duration,Bytes(key.pixels.size())};
        std::vector<uint32_t> vectors(p.vectors.size());for(size_t i=0;i<vectors.size();++i)vectors[i]=std::bit_cast<uint32_t>(p.vectors[i]);
        std::array<GpuPlane,3> gpu;for(unsigned c=0;c<3;++c)gpu[c]={planes[c],c?w/2:w,c?h/2:h};
        if(gpuReconstructMotion(key,vectors,p.bias,keep,p.affine,gpu,version>=15?step:0,accelerated.pixels,inverse,true,version==16)){
            timing.mark("decode.motion.resident_reconstruction");return accelerated;}
    }
    // The GPU prediction and canonical CPU inverse have no shared writable
    // data. Submit the prediction first, then use its fence wait for inversion.
    auto result=warp(key,p,workers,keep,inverse);result.pts=packet.pts;result.duration=packet.duration;
    timing.mark("decode.motion.warp");
    result=reconstructResidual(std::move(result),planes,w,h,workers,keep,blockTransform,inverseDone);
    timing.mark("decode.motion.synthesis_total");
    if(version>=15)filterReference(result,step,workers,keep,version==16);timing.mark("decode.motion.reference_filter");return result;
}
}
