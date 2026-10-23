#pragma once
#include "fajk/types.hpp"
#include "fajk/hardware.hpp"
#include <array>
#include <immintrin.h>
namespace fajk {
bool hasAvx2();
uint32_t r12RgbCompactRowAvx2(const uint8_t*,const uint8_t*,const uint8_t*,uint8_t*,uint32_t,uint32_t,float,float);
inline double r12ReferenceError(const Frame& reference,const Frame& image,uint32_t threads){
    require(reference.width==image.width&&reference.height==image.height,"R12 comparison dimensions differ.");std::vector<double> rows(reference.height);
    parallelFor(reference.height,threads,[&](uint32_t y){double total=0;for(uint32_t x=0;x<reference.width;++x){const size_t at=(size_t(y)*reference.width+x)*4;for(int c=0;c<3;++c){const double difference=int(reference.pixels[at+c])-int(image.pixels[at+c]);total+=difference*difference;for(auto n:{size_t(y)*reference.width+(x?x-1:x),size_t(y?y-1:y)*reference.width+x}){const double gradient=(int(reference.pixels[at+c])-int(reference.pixels[n*4+c]))-(int(image.pixels[at+c])-int(image.pixels[n*4+c]));total+=2*gradient*gradient;}}}rows[y]=total;});double result=0;for(auto value:rows)result+=value;return result;
}
inline double r12ReferenceCoarseError(const Frame& reference,const Frame& image,uint32_t threads){
    require(reference.width==image.width&&reference.height==image.height,"R12 comparison dimensions differ.");
    const uint32_t rows=(reference.height+7)/8;std::vector<double> errors(rows);
    parallelFor(rows,threads,[&](uint32_t row){double error=0;
        for(uint32_t x=0;x<reference.width;x+=8){std::array<int,3> sums{};unsigned count=0;
            for(uint32_t y=row*8;y<std::min(reference.height,row*8+8);++y)for(uint32_t xx=x;xx<std::min(reference.width,x+8);++xx){
                const size_t at=(size_t(y)*reference.width+xx)*4;
                for(int c=0;c<3;++c)sums[c]+=int(reference.pixels[at+c])-int(image.pixels[at+c]);++count;
            }
            for(auto sum:sums)error+=double(sum)*sum/count;
        }errors[row]=error;
    });double result=0;for(auto error:errors)result+=error;return result;
}
// Encoder-only structural proxy at 1:4 scale; this is not SSIMULACRA2.
inline double r12ReferenceStructureError(const Frame& reference,const Frame& image,uint32_t threads){
    require(reference.width==image.width&&reference.height==image.height,"R12 comparison dimensions differ.");
    const uint32_t w=(reference.width+3)/4,h=(reference.height+3)/4;
    std::array<std::vector<float>,2> planes;for(auto& plane:planes)plane.resize(size_t(w)*h);
    parallelFor(h,threads,[&](uint32_t y){for(uint32_t x=0;x<w;++x){double sums[2]{};unsigned count=0;
        for(uint32_t yy=y*4;yy<std::min(reference.height,y*4+4);++yy)for(uint32_t xx=x*4;xx<std::min(reference.width,x*4+4);++xx){
            const size_t at=(size_t(yy)*reference.width+xx)*4;
            const uint8_t* pixels[2]={reference.pixels.data()+at,image.pixels.data()+at};
            for(int p=0;p<2;++p)sums[p]+=(.299*pixels[p][0]+.587*pixels[p][1]+.114*pixels[p][2])/255.;++count;
        }for(int p=0;p<2;++p)planes[p][size_t(y)*w+x]=float(sums[p]/count);
    }});
    std::vector<double> errors(h);constexpr double weights[5]={1/16.,4/16.,6/16.,4/16.,1/16.};
    parallelFor(h,threads,[&](uint32_t y){double total=0;for(uint32_t x=0;x<w;++x){double a=0,b=0,aa=0,bb=0,ab=0;
        for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx){const auto yy=uint32_t(std::clamp(int(y)+dy,0,int(h)-1)),xx=uint32_t(std::clamp(int(x)+dx,0,int(w)-1));
            const double weight=weights[dy+2]*weights[dx+2],v=planes[0][size_t(yy)*w+xx],u=planes[1][size_t(yy)*w+xx];
            a+=weight*v;b+=weight*u;aa+=weight*v*v;bb+=weight*u*u;ab+=weight*v*u;
        }
        const double variance=std::max(0.,aa-a*a)+std::max(0.,bb-b*b),difference=a-b;
        const double error=std::max(0.,1-(1-difference*difference)*(2*(ab-a*b)+.0009)/(variance+.0009));
        total+=error+.25*error*error*error*error;
    }errors[y]=total;});double result=0;for(auto error:errors)result+=error;return result;
}
inline void guardR12Details(const Frame& reference,const Frame& unfiltered,const Frame& filtered,Bytes& map,uint32_t threads,float gradientWeight){
    require(reference.width==unfiltered.width&&reference.height==unfiltered.height&&reference.width==filtered.width&&reference.height==filtered.height,"R12 detail guard dimensions differ.");const uint32_t cols=(reference.width+63)/64,rows=(reference.height+63)/64;require(map.size()==size_t(cols)*rows*2,"Invalid R12 detail guard map.");
    parallelFor(rows,threads,[&](uint32_t ty){for(uint32_t tx=0;tx<cols;++tx){double before=0,after=0,beforeGradient=0,afterGradient=0;
        for(uint32_t y=ty*64;y<std::min(reference.height,(ty+1)*64);++y)for(uint32_t x=tx*64;x<std::min(reference.width,(tx+1)*64);++x){const size_t at=(size_t(y)*reference.width+x)*4;for(int c=0;c<3;++c){const double a=int(unfiltered.pixels[at+c])-int(reference.pixels[at+c]),b=int(filtered.pixels[at+c])-int(reference.pixels[at+c]);before+=a*a;after+=b*b;
            for(auto n:{size_t(y)*reference.width+(x?x-1:x),size_t(y?y-1:y)*reference.width+x}){const double target=int(reference.pixels[at+c])-int(reference.pixels[n*4+c]),da=int(unfiltered.pixels[at+c])-int(unfiltered.pixels[n*4+c])-target,db=int(filtered.pixels[at+c])-int(filtered.pixels[n*4+c])-target;before+=gradientWeight*da*da;after+=gradientWeight*db*db;beforeGradient+=da*da;afterGradient+=db*db;}
        }}if(after>=before||afterGradient>beforeGradient){const size_t at=(size_t(ty)*cols+tx)*2;map[at]=map[at+1]=32;}
    }});
}
inline constexpr std::array<float,5> r12EdgeGains{0.f,.25f,.5f,1.f,1.5f};
inline std::array<uint8_t,3> r12EdgePixel(std::span<const uint8_t> source,uint32_t width,uint32_t height,uint32_t x,uint32_t y,float gain){
    const size_t at=(size_t(y)*width+x)*4;std::array<float,3> mean{},delta{},lo{},hi{};for(int c=0;c<3;++c)lo[c]=hi[c]=source[at+c];
    for(int distance:{1,2})for(const auto& d:std::array<std::array<int,2>,4>{{{1,0},{-1,0},{0,1},{0,-1}}}){const auto xx=uint32_t(std::clamp(int(x)+d[0]*distance,0,int(width)-1)),yy=uint32_t(std::clamp(int(y)+d[1]*distance,0,int(height)-1));const size_t n=(size_t(yy)*width+xx)*4;for(int c=0;c<3;++c){mean[c]+=source[n+c]*.125f;lo[c]=std::min(lo[c],float(source[n+c]));hi[c]=std::max(hi[c],float(source[n+c]));}}
    float fraction=1;for(int c=0;c<3;++c){delta[c]=(source[at+c]-mean[c])*gain;if(delta[c]>0)fraction=std::min(fraction,(hi[c]-source[at+c])/delta[c]);else if(delta[c]<0)fraction=std::min(fraction,(lo[c]-source[at+c])/delta[c]);}
    std::array<uint8_t,3> result{};for(int c=0;c<3;++c)result[c]=uint8_t(std::clamp(int(source[at+c]+fraction*delta[c]+.5f),0,255));return result;
}
inline Bytes fitR12Edges(const Frame& original,const Frame& reconstructed,uint32_t threads){
    require(original.width==reconstructed.width&&original.height==reconstructed.height,"R12 edge fit dimensions differ.");const uint32_t cols=(original.width+63)/64,rows=(original.height+63)/64;Bytes map(size_t(cols)*rows);
    parallelFor(rows,threads,[&](uint32_t ty){for(uint32_t tx=0;tx<cols;++tx){std::array<double,5> errors{};
        for(uint32_t y=ty*64;y<std::min(original.height,(ty+1)*64);++y)for(uint32_t x=tx*64;x<std::min(original.width,(tx+1)*64);++x){const size_t at=(size_t(y)*original.width+x)*4;int gradient=0;const size_t neighbors[2]={size_t(y)*original.width+(x?x-1:x),size_t(y?y-1:y)*original.width+x};for(auto n:neighbors)for(int c=0;c<3;++c)gradient=std::max(gradient,std::abs(int(original.pixels[n*4+c])-int(original.pixels[at+c])));const double weight=gradient>=24?4.:1.;
            for(size_t g=0;g<r12EdgeGains.size();++g){const auto pixel=r12EdgePixel(reconstructed.pixels,original.width,original.height,x,y,r12EdgeGains[g]);for(int c=0;c<3;++c){const double d=int(pixel[c])-int(original.pixels[at+c]);errors[g]+=weight*d*d;}}
        }size_t best=0;for(size_t g=1;g<errors.size();++g)if(errors[g]+32<errors[best])best=g;map[size_t(ty)*cols+tx]=uint8_t(best);
    }});return map;
}
inline void restoreR12Edges(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t threads,std::span<const uint8_t> map){
    require(pixels.size()==pixelBytes(width,height)&&map.size()==size_t((width+63)/64)*((height+63)/64),"Invalid R12 edge map.");for(auto value:map)require(value<r12EdgeGains.size(),"Invalid R12 edge gain.");const uint32_t cols=(width+63)/64;const Bytes source(pixels.begin(),pixels.end());
    parallelFor(height,threads,[&](uint32_t y){for(uint32_t x=0;x<width;++x){const auto gain=map[size_t(y/64)*cols+x/64];if(!gain)continue;const auto pixel=r12EdgePixel(source,width,height,x,y,r12EdgeGains[gain]);const size_t at=(size_t(y)*width+x)*4;for(int c=0;c<3;++c)pixels[at+c]=pixel[c];}});
}
// All channels use the same range gate. An isoluminant color boundary must
// stop restoration as well as a luminance boundary. Alpha is never filtered.
inline void restoreRgb(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t threads,uint8_t threshold,float blend=1.f,uint8_t passes=2,bool compact=false,uint8_t spacing=1,std::span<const uint8_t> externalSource={}){
    require(externalSource.empty()||(externalSource.size()==pixels.size()&&passes==1&&externalSource.data()!=pixels.data()),"Invalid R12 RGB source.");require(pixels.size()==pixelBytes(width,height)&&threshold<=64&&blend>=0&&blend<=1&&passes>=1&&passes<=2&&(spacing==1||spacing==2||spacing==4),"Invalid R12 RGB reconstruction.");if(!threshold)return;
    const std::array<std::array<int,3>,12> taps={{{1,0,10},{-1,0,10},{0,1,10},{0,-1,10},{1,1,7},{-1,1,7},{1,-1,7},{-1,-1,7},{2,0,5},{-2,0,5},{0,2,5},{0,-2,5}}};
    thread_local Bytes cache;Bytes owned;auto& snapshot=pixels.size()<=64*1024*1024?cache:owned;if(externalSource.empty())snapshot.resize(pixels.size());const std::span<const uint8_t> source=externalSource.empty()?std::span<const uint8_t>(snapshot):externalSource;const bool avx=compact&&spacing==1&&hasAvx2();const auto zero=_mm_setzero_si128(),mask=_mm_set1_epi32(255);const auto sign=_mm_set1_ps(-0.f),one=_mm_set1_ps(1.f),inv=_mm_set1_ps(1.f/threshold);
    auto channels=[&](const uint8_t* p,__m128 (&v)[3]){const auto rgba=_mm_loadu_si128(reinterpret_cast<const __m128i*>(p));for(int c=0;c<3;++c)v[c]=_mm_cvtepi32_ps(_mm_and_si128(c==0?rgba:c==1?_mm_srli_epi32(rgba,8):_mm_srli_epi32(rgba,16),mask));};
    for(int pass=0;pass<passes;++pass){if(externalSource.empty())parallelFor(height,threads,[&](uint32_t y){std::copy_n(pixels.data()+size_t(y)*width*4,size_t(width)*4,snapshot.data()+size_t(y)*width*4);});
        parallelFor(height,threads,[&](uint32_t y){uint32_t x=0;
            auto scalar=[&](uint32_t x){const size_t at=(size_t(y)*width+x)*4;float sum[3]={float(source[at])*4,float(source[at+1])*4,float(source[at+2])*4},total=4;
                for(const auto& tap:taps){if(compact&&(std::abs(tap[0])==2||std::abs(tap[1])==2))continue;const auto xx=uint32_t(std::clamp(int(x)+tap[0]*spacing,0,int(width)-1)),yy=uint32_t(std::clamp(int(y)+tap[1]*spacing,0,int(height)-1));const size_t n=(size_t(yy)*width+xx)*4;float difference=0;for(int c=0;c<3;++c)difference=std::max(difference,float(std::abs(int(source[n+c])-int(source[at+c]))));const float gate=std::max(0.f,1-difference/threshold),weight=gate*gate*tap[2]*.1f;total+=weight;for(int c=0;c<3;++c)sum[c]+=weight*source[n+c];}
                for(int c=0;c<3;++c){const float average=sum[c]/total,value=blend<1?source[at+c]+blend*(average-source[at+c]):average;pixels[at+c]=uint8_t(std::clamp(int(value+.5f),0,255));}pixels[at+3]=source[at+3];};
            for(;x<std::min(uint32_t(2*spacing),width);++x)scalar(x);
            if(avx)x=r12RgbCompactRowAvx2(source.data()+size_t(y?y-1:y)*width*4,source.data()+size_t(y)*width*4,source.data()+size_t(std::min(height-1,y+1))*width*4,pixels.data()+size_t(y)*width*4,x,width,threshold,blend);
            for(;x+2*spacing+3<width;x+=4){const size_t at=(size_t(y)*width+x)*4;__m128 center[3],sum[3];channels(source.data()+at,center);for(int c=0;c<3;++c)sum[c]=_mm_mul_ps(center[c],_mm_set1_ps(4.f));auto total=_mm_set1_ps(4.f);
                for(const auto& tap:taps){if(compact&&(std::abs(tap[0])==2||std::abs(tap[1])==2))continue;const int yy=std::clamp(int(y)+tap[1]*spacing,0,int(height)-1);__m128 neighbor[3];channels(source.data()+(size_t(yy)*width+int(x)+tap[0]*spacing)*4,neighbor);auto difference=_mm_setzero_ps();for(int c=0;c<3;++c)difference=_mm_max_ps(difference,_mm_andnot_ps(sign,_mm_sub_ps(neighbor[c],center[c])));const auto gate=_mm_max_ps(_mm_setzero_ps(),_mm_sub_ps(one,_mm_mul_ps(difference,inv))),weight=_mm_mul_ps(_mm_mul_ps(gate,gate),_mm_set1_ps(tap[2]*.1f));total=_mm_add_ps(total,weight);for(int c=0;c<3;++c)sum[c]=_mm_add_ps(sum[c],_mm_mul_ps(neighbor[c],weight));}
                __m128i result[3];for(int c=0;c<3;++c){auto average=_mm_div_ps(sum[c],total);if(blend<1)average=_mm_add_ps(center[c],_mm_mul_ps(_mm_sub_ps(average,center[c]),_mm_set1_ps(blend)));result[c]=_mm_cvttps_epi32(_mm_add_ps(average,_mm_set1_ps(.5f)));}const auto alpha=_mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(source.data()+at)),_mm_set1_epi32(int(0xff000000u)));const auto rgb=_mm_or_si128(result[0],_mm_or_si128(_mm_slli_epi32(result[1],8),_mm_slli_epi32(result[2],16)));_mm_storeu_si128(reinterpret_cast<__m128i*>(pixels.data()+at),_mm_or_si128(rgb,alpha));
            }for(;x<width;++x)scalar(x);
        });
    }
}
}
