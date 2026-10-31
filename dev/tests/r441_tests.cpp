#include "fajk/codec.hpp"
#include "../src/reconstruction441.hpp"
#include "../src/decode_internal.hpp"
#include "../src/fast_reconstruction.hpp"
#include "../src/cleanup_quality.hpp"
#include "../src/artifact_reconstruction.hpp"
#include "../src/flat_regions.hpp"
#include "../src/text_guard.hpp"
#include <iostream>
#include <source_location>
using namespace fajk;
void check(bool value,std::source_location at=std::source_location::current()){
    if(!value)throw std::runtime_error("R441 regression failed at line "+std::to_string(at.line()));
}
template<class F>void rejected(F&& f){bool threw=false;try{f();}catch(const std::exception&){threw=true;}check(threw);}
int main(){try{
    // Quality fades once when creating display controls; manual mode bypasses
    // both that envelope and density attenuation without changing other knobs.
    for(uint32_t raw:{0u,50u,100u,200u}){
        check(skirtQualityIntensity(raw,1,true)==raw);
        check(skirtQualityIntensity(raw,10,true)==raw);
        check(skirtQualityIntensity(raw,30,true)==raw/2);
        check(skirtQualityIntensity(raw,50,true)==0);
        check(skirtQualityIntensity(raw,100,true)==0);
        for(int q=1;q<=100;++q)check(skirtQualityIntensity(raw,float(q),false)==raw);
    }
    rejected([]{skirtQualityIntensity(50,std::numeric_limits<float>::quiet_NaN(),true);});
    for(int q=10;q<50;++q)check(skirtQualityIntensity(200,float(q+1),true)<=skirtQualityIntensity(200,float(q),true));
    for(uint32_t budget:{30000u,80000u,200000u}){
        auto q=cleanupQuality(budget,2560,1440);Reconstruction441 s;s.intensity=50;
        check(skirtBlend(s,q)==q.contours*.5f);s.fixedIntensity=true;
        check(skirtBlend(s,q)==.5f);
    }
    check(contourGuideSpacing(0,2)==8);
    check(contourGuideSpacing(0,.7f)==16);
    check(contourGuideSpacing(.15f,.7f)==8);
    check(contourGuideSpacing(.5f,.1f)==8);
    // Protect decoded glyphs before geometry changes them, while allowing a
    // separate single diagonal outline and chromatic silhouette to be cleaned.
    {
        constexpr uint32_t w=193,h=97;Bytes native(pixelBytes(w,h),255);
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){auto* p=native.data()+(size_t(y)*w+x)*4;p[0]=p[1]=p[2]=24;}
        for(uint32_t y=20;y<34;++y)for(uint32_t x=20;x<92;++x){const uint32_t letter=(x-20)%12;if(letter==0||letter==6||y==26){auto* p=native.data()+(size_t(y)*w+x)*4;p[0]=p[1]=p[2]=230;}}
        for(uint32_t y=60;y<85;++y){auto* p=native.data()+(size_t(y)*w+y+45)*4;p[0]=p[1]=p[2]=230;}
        auto protectedText=captureDecodedText(native,w,h,1);const auto parallel=captureDecodedText(native,w,h,3);
        check(!protectedText.ranges.empty()&&protectedText.pixels==parallel.pixels);
        auto candidate=native;for(size_t at=0;at<candidate.size();at+=4)candidate[at]=candidate[at+1]=candidate[at+2]=0;
        refreshDecodedTextFallback(protectedText,candidate,w,h);
        restoreDecodedText(candidate,protectedText,w,h);
        check(candidate[(size_t(25)*w+20)*4]==230);
        check(candidate[(size_t(75)*w+120)*4]==0);
        rejected([&]{restoreDecodedText(candidate,protectedText,w-1,h);});
        check(protectedText.ranges.size()==parallel.ranges.size());
        size_t end=0,total=0;
        for(size_t i=0;i<protectedText.ranges.size();++i){const auto& r=protectedText.ranges[i];const auto& p=parallel.ranges[i];
            check(r.offset>=end&&r.offset==p.offset&&r.count==p.count&&r.early==p.early);end=r.offset+r.count;total+=r.count;}
        check(total==protectedText.pixels.size());
        auto invalid=protectedText;invalid.ranges.back().count+=candidate.size();const auto unchanged=candidate;
        rejected([&]{restoreDecodedText(candidate,invalid,w,h);});check(candidate==unchanged);
        const auto saved=invalid.pixels;rejected([&]{refreshDecodedTextFallback(invalid,candidate,w,h);});check(invalid.pixels==saved);
    }
    // Compression can bend a smooth silhouette without changing its colors.
    // Require reconstruction to reduce its positional error, also after a
    // uniform brightness shift; a darker silhouette is not a weaker contour.
    {
        constexpr uint32_t w=193,h=145;Bytes dark(pixelBytes(w,h),255),light=dark;
        auto boundary=[](float y){return 81.f+.13f*y+.0008f*(y-72)*(y-72);};
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
            const float distorted=boundary(float(y))+1.3f*std::sin(.63f*y)+.45f*std::sin(1.71f*y);
            const float t=std::clamp(float(x)-distorted+.5f,0.f,1.f);
            for(int c=0;c<3;++c){const float a=std::array<float,3>{34,20,37}[c],b=std::array<float,3>{97,12,150}[c];const auto at=(size_t(y)*w+x)*4+c;dark[at]=uint8_t(a+t*(b-a)+.5f);light[at]=dark[at]+60;}
        }
        const auto original=dark;
        artifactReconstruction::restore(dark,w,h,1,30000,0,200,2560,1440);
        artifactReconstruction::restore(light,w,h,3,30000,0,200,2560,1440);
        double before=0,after=0,brightnessError=0;size_t count=0;
        for(uint32_t y=20;y<h-20;++y){
            double a=0,b=0;for(uint32_t x=0;x<w;++x){const auto at=(size_t(y)*w+x)*4;a+=(original[at+2]-37)/113.;b+=(dark[at+2]-37)/113.;for(int c=0;c<3;++c){brightnessError+=std::abs(int(light[at+c])-int(dark[at+c])-60);++count;}}
            const double expected=w-.5-boundary(float(y));before+=std::abs(a-expected);after+=std::abs(b-expected);
        }
        std::cout<<"Silhouette position error "<<before<<" -> "<<after<<", brightness difference "<<brightnessError/count<<'\n';
        require(after<before*.8,"Contour flow did not reduce positional waviness.");
        require(brightnessError/count<.1,"Dark and bright contour behavior diverged.");
    }
    // A winding silhouette changes direction substantially between guides.
    // Measure actual render weights near the analytic contour, not just the
    // number of color models. A disabled diagnostic must be completely black.
    {
        constexpr uint32_t w=193,h=129;Frame native{w,h,0,1,Bytes(pixelBytes(w,h),255)};
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
            const float edge=84.f+13.f*std::sin(.20f*y);
            const float t=std::clamp(float(x)-edge+.5f,0.f,1.f);
            for(int c=0;c<3;++c){const float a=std::array<float,3>{90,15,135}[c],b=std::array<float,3>{22,17,29}[c];native.pixels[(size_t(y)*w+x)*4+c]=uint8_t(a+t*(b-a)+.5f);}
        }
        native.pixels[(size_t(64)*w+85)*4+3]=127;
        const auto before=contourEffectMap(native,8,2,false),after=contourEffectMap(native,8,2,true),off=contourEffectMap(native,8,0,true);
        size_t oldCoverage=0,newCoverage=0;double oldWeight=0,newWeight=0;
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
            const auto at=(size_t(y)*w+x)*4;
            check(off.pixels[at]==0&&off.pixels[at+1]==0&&off.pixels[at+2]==0&&off.pixels[at+3]==255);
            check(after.pixels[at+3]==255);
            if(y>12&&y<h-12&&std::abs(float(x)-84.f-13.f*std::sin(.20f*y))<3){oldCoverage+=before.pixels[at+1]>=8;newCoverage+=after.pixels[at+1]>=8;oldWeight+=before.pixels[at+1];newWeight+=after.pixels[at+1];}
        }
        std::cout<<"Curved contour coverage "<<oldCoverage<<" -> "<<newCoverage<<", weight "<<oldWeight<<" -> "<<newWeight<<'\n';
        require(oldCoverage>0&&newCoverage>oldCoverage*1.05,"Curved contour coverage did not improve.");
    }
    // Low-contrast colored silhouettes must not disappear behind an absolute
    // 24-level edge gate. Use an analytic curved contour with deliberately
    // quantized coverage; require a real reduction of its coverage error.
    {
        constexpr uint32_t w=97,h=79;Bytes reference(pixelBytes(w,h),255),aliased=reference;
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
            const float distance=float(x)-39.f-.21f*y-2.f*std::sin(.05f*y);
            const float ideal=std::clamp(distance+.5f,0.f,1.f),coarse=distance>=0?1.f:0.f;
            for(int c=0;c<3;++c){const float low=std::array<float,3>{48,23,75}[c],delta=std::array<float,3>{16,2,-22}[c];const auto at=(size_t(y)*w+x)*4+c;reference[at]=uint8_t(low+ideal*delta+.5f);aliased[at]=uint8_t(low+coarse*delta+.5f);}
        }
        auto restored=aliased,parallel=aliased;
        artifactReconstruction::restore(restored,w,h,1,30000,0,200,2560,1440);
        artifactReconstruction::restore(parallel,w,h,3,30000,0,200,2560,1440);
        check(restored==parallel);
        double before=0,after=0;size_t changed=0;
        for(uint32_t y=12;y<h-12;++y)for(uint32_t x=12;x<w-12;++x){
            const auto at=(size_t(y)*w+x)*4;check(restored[at+3]==255);
            for(int c=0;c<3;++c){before+=std::abs(int(aliased[at+c])-reference[at+c]);after+=std::abs(int(restored[at+c])-reference[at+c]);changed+=restored[at+c]!=aliased[at+c];}
        }
        check(changed>20&&after<before*.99);
    }
    // A broad soft edge can have <12 RGB levels per bilinear cell. The old
    // four-tap contrast gate ignored it. Wider contrast support must narrow
    // that transition while staying inside the four central color bounds.
    {
        constexpr uint32_t iw=17,ih=13,w=35,h=27;
        Frame source{iw,ih,0,1,Bytes(pixelBytes(iw,ih),255)};
        for(uint32_t y=0;y<ih;++y)for(uint32_t x=0;x<iw;++x){const int position=std::clamp(int(x)-3,0,10);auto* p=source.pixels.data()+(size_t(y)*iw+x)*4;p[0]=uint8_t(40+8*position);p[1]=30;p[2]=uint8_t(170-8*position);}
        Reconstruction441 disabled;disabled.intensity=disabled.antialias=disabled.contrast=0;disabled.protectText=false;
        auto a=source,b=source,c=source;
        const auto plain=fastReconstruct441(a,w,h,1,150000,disabled);
        auto enabled=disabled;enabled.contrast=100;
        const auto sharp=fastReconstruct441(b,w,h,1,150000,enabled);
        Bytes guarded(pixelBytes(w,h)+32,197);
        fastReconstruct441(c,w,h,3,150000,enabled,std::span(guarded).subspan(16,pixelBytes(w,h)));
        check(sharp.pixels!=plain.pixels&&std::equal(sharp.pixels.begin(),sharp.pixels.end(),guarded.begin()+16));
        for(int i=0;i<16;++i)check(guarded[i]==197&&guarded[guarded.size()-1-i]==197);
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
            const int ix=int(std::floor((x+.5)*iw/w-.5)),iy=int(std::floor((y+.5)*ih/h-.5));
            const auto i=(size_t(y)*w+x)*4;check(sharp.pixels[i+3]==255);
            for(int channel=0;channel<3;++channel){int lo=255,hi=0;
                for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){const int value=source.pixels[(size_t(std::clamp(iy+dy,0,int(ih)-1))*iw+std::clamp(ix+dx,0,int(iw)-1))*4+channel];lo=std::min(lo,value);hi=std::max(hi,value);}
                check(sharp.pixels[i+channel]>=lo&&sharp.pixels[i+channel]<=hi);
            }
            if(x)check(sharp.pixels[i]>=sharp.pixels[i-4]);
        }
    }
    // Color leakage outside a straight contour is not part of its coverage.
    // Repair the contaminated band on both sides without touching distant
    // shading, alpha, or depending on scheduling. Use reference density rather
    // than interpreting 30 kB as the budget for this tiny synthetic image.
    {
        constexpr uint32_t w=193,h=129;Bytes reference(pixelBytes(w,h),255),noisy;
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
            const float distance=float(x)-76.f-.17f*y;
            const float t=std::clamp(distance+.5f,0.f,1.f);
            const size_t i=(size_t(y)*w+x)*4;
            for(int c=0;c<3;++c){const float a=std::array<float,3>{90,15,135}[c],b=std::array<float,3>{210,30,20}[c];reference[i+c]=uint8_t(a+t*(b-a)+.5f);}
        }
        noisy=reference;
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
            const float distance=float(x)-76.f-.17f*y;
            if(std::abs(distance)<3||std::abs(distance)>8)continue;
            const float spill=(distance<0?1.f:-1.f)*(.08f+.03f*std::sin(float(y)*.7f));
            const size_t i=(size_t(y)*w+x)*4;
            for(int c=0;c<3;++c){const float delta=std::array<float,3>{120,15,-115}[c];noisy[i+c]=uint8_t(std::clamp(int(reference[i+c]+spill*delta+.5f),0,255));}
        }
        auto a=noisy,b=noisy;
        artifactReconstruction::restore(a,w,h,1,30000,0,200,2560,1440);
        artifactReconstruction::restore(b,w,h,3,30000,0,200,2560,1440);
        check(a==b);
        double before=0,after=0;size_t count=0;
        for(uint32_t y=20;y<h-20;++y)for(uint32_t x=20;x<w-20;++x){
            const float d=std::abs(float(x)-76.f-.17f*y);const auto i=(size_t(y)*w+x)*4;
            check(a[i+3]==255);
            if(d>=4&&d<=7)for(int c=0;c<3;++c){before+=std::abs(int(noisy[i+c])-reference[i+c]);after+=std::abs(int(a[i+c])-reference[i+c]);++count;}
            if(d>24)for(int c=0;c<3;++c)check(a[i+c]==reference[i+c]);
        }
        check(count>0&&after<before*.8);
    }
    // The inexpensive weak filter must preserve alpha and local color bounds,
    // including narrow images, SIMD tails and concurrent execution.
    for(uint32_t w:{1u,7u,8u,17u,113u}){
        constexpr uint32_t h=19;Bytes original(pixelBytes(w,h),255);
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const auto i=(size_t(y)*w+x)*4;
            original[i]=uint8_t(40+x*7%180);original[i+1]=uint8_t(60+y*5);original[i+2]=uint8_t(90+(x+y)%7);
            if((x+y)%11==0)original[i+3]=128;
        }
        auto a=original,b=original;recursiveContourCleanup(a,w,h,1,.15f,0,{},.03f);
        recursiveContourCleanup(b,w,h,3,.15f,0,{},.03f);check(a==b);
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const auto i=(size_t(y)*w+x)*4;check(a[i+3]==original[i+3]);
            for(int c=0;c<3;++c){int lo=255,hi=0;
                for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){auto v=original[(size_t(std::clamp(int(y)+dy,0,int(h)-1))*w+std::clamp(int(x)+dx,0,int(w)-1))*4+c];lo=std::min(lo,int(v));hi=std::max(hi,int(v));}
                check(a[i+c]>=lo&&a[i+c]<=hi);if(original[i+3]!=255)check(a[i+c]==original[i+c]);
            }
        }
    }
    // Manual sharpening remains active even when automatic high-rate cleanup
    // has faded out. It must not add halos, move alpha or depend on workers.
    {
        constexpr uint32_t w=53,h=31;Bytes p(pixelBytes(w,h),255);
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const float t=std::clamp((float(x)-20)/8,0.f,1.f);auto i=(size_t(y)*w+x)*4;p[i]=uint8_t(40+160*t);p[i+1]=30;p[i+2]=uint8_t(160-120*t);}
        Reconstruction441 s;s.intensity=s.antialias=0;s.protectText=false;s.contrast=50;
        auto a=p,b=p;reconstruct441(a,w,h,1,150000,s);reconstruct441(b,w,h,3,150000,s);check(a!=p&&a==b);
        for(size_t i=0;i<a.size();i+=4){check(a[i]>=40&&a[i]<=200&&a[i+1]==30&&a[i+2]>=40&&a[i+2]<=160&&a[i+3]==255);}
        auto off=p;s.contrast=0;reconstruct441(off,w,h,3,150000,s);check(off==p);
        artifactReconstruction::restore(off,w,h,3,150000,0,200);check(off==p);
    }
    // Directional regularization must reduce contour jitter, not merely blur
    // an already straight transition. Odd sizes exercise SIMD tails and tiles.
    {
        constexpr uint32_t w=117,h=109;
        auto edge=[&](bool jitter){Bytes p(pixelBytes(w,h),255);
            for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
                const float boundary=jitter?45.f+.17f*y+(y%4<2?1.2f:-1.2f):55.f;
                const float coverage=std::clamp((float(x)-boundary)*.5f+.5f,0.f,1.f);
                const size_t i=(size_t(y)*w+x)*4;
                p[i]=uint8_t(40+160*coverage);p[i+1]=30;p[i+2]=uint8_t(160-120*coverage);
            }return p;};
        auto straight=edge(false),straightCurve=straight;
        recursiveContourCleanup(straight,w,h,3,1,0);
        recursiveContourCleanup(straightCurve,w,h,3,1,0,{},.4f);
        check(straight==straightCurve);
        auto zigzag=edge(true),curved=zigzag,single=zigzag;
        auto unfused=zigzag,fused=zigzag;
        recursiveContourCleanup(unfused,w,h,3,1,0,{},0,{},false);
        recursiveContourCleanup(fused,w,h,3,1,0,{},0,{},true);
        for(size_t i=0;i<fused.size();++i)check(std::abs(int(fused[i])-int(unfused[i]))<=1);
        recursiveContourCleanup(zigzag,w,h,3,1,0);
        recursiveContourCleanup(curved,w,h,3,1,0,{},.4f);
        recursiveContourCleanup(single,w,h,1,1,0,{},.4f);check(curved==single);
        auto roughness=[&](const Bytes& p){double sum=0;
            auto mass=[&](uint32_t y){double value=0;for(uint32_t x=10;x<w-10;++x)value+=p[(size_t(y)*w+x)*4];return value;};
            for(uint32_t y=8;y<h-8;++y){const double d=mass(y-1)-2*mass(y)+mass(y+1);sum+=d*d;}return sum;};
        check(roughness(curved)<roughness(zigzag));
        for(size_t i=0;i<curved.size();i+=4){check(curved[i]>=40&&curved[i]<=200);check(curved[i+1]==30);check(curved[i+2]>=40&&curved[i+2]<=160);check(curved[i+3]==255);}
        float previous=1;
        const auto lowQuality=cleanupQuality(30000,2560,1440),moderateQuality=cleanupQuality(80000,2560,1440);
        check(lowQuality.contours==1&&lowQuality.antialias==1);
        check(moderateQuality.contours>.65f&&moderateQuality.contours<.66f&&moderateQuality.antialias>=.11f);
        for(uint32_t bytes=30000;bytes<=300000;bytes+=1000){
            const auto q=cleanupQuality(bytes,2560,1440),sameDensity=cleanupQuality(bytes*4,5120,2880);
            check(q.contours<=previous&&q.contours==sameDensity.contours);previous=q.contours;
            if(bytes>=150000)check(q.contours>=.45f&&q.antialias==0);
        }
    }
    Frame source{129,97,17,23,Bytes(pixelBytes(129,97),255)};
    for(uint32_t y=0;y<source.height;++y)for(uint32_t x=0;x<source.width;++x){auto* p=source.pixels.data()+(size_t(y)*source.width+x)*4;const bool side=x*3>y*2+30;const int ripple=(x*13+y*7)%9-4;
        p[0]=uint8_t((side?175:65)+ripple);p[1]=uint8_t((side?45:20)+ripple);p[2]=uint8_t((side?35:145)+ripple);
    }
    auto codec=makeCodec(CodecId::Fajk);codec->setSharpening(0);codec->setThreads(3);
    EncodeOptions options;options.revision=EncoderRevision::Experimental011441R441;options.targetBytes=12000;options.quality=100;options.chroma420=true;options.threads=3;
    const auto packet=codec->encode(source,options);check(packet.data.size()<=options.targetBytes&&packet.data[4]==reconstruction441Version);
    const auto view=read441(packet.data,source.width,source.height);check(view.budget==packet.data.size()+168);check(view.settings==options.reconstruction441&&view.child[4]==129&&view.child[5]==12);
    const auto image=codec->decode(packet,source.width,source.height);check(image.pts==17&&image.duration==23);
    Bytes buffer(source.pixels.size()+32,197);decodeFrameToBuffer(packet,source.width,source.height,1,100,std::span(buffer).subspan(16,source.pixels.size()),0,true);
    check(std::equal(image.pixels.begin(),image.pixels.end(),buffer.begin()+16));for(size_t i=0;i<16;++i)check(buffer[i]==197&&buffer[buffer.size()-1-i]==197);
    auto off=packet;auto settings=view.settings;settings.intensity=settings.antialias=settings.contrast=0;update441(off,settings);
    Packet base{Bytes(view.child.begin(),view.child.end()),17,23};check(codec->decode(off,source.width,source.height).pixels==codec->decode(base,source.width,source.height).pixels);
    // Every reconstruction-only edit preserves the entire compressed child.
    check(packet.data[30]==0&&packet.data[31]==0); // 100/100 preserves the original header bytes.
    for(int control=0;control<9;++control){auto edited=packet;auto s=view.settings;
        if(control==0)s.intensity=0;if(control==1)s.strongerTangents=100;if(control==2)s.antialias=100;if(control==3)s.contrast=70;if(control==4)s.style=1;if(control==5)s.protectText=false;
        if(control==6)s.fixedIntensity=true;
        if(control==7)s.shapes.tolerance=175;
        if(control==8)s.shapes.minimumSize=50;
        update441(edited,s);auto parsed=read441(edited.data,source.width,source.height);check(parsed.settings==s&&std::equal(parsed.child.begin(),parsed.child.end(),view.child.begin(),view.child.end()));
        options.reconstruction441=s;check(codec->encode(source,options).data==edited.data);
    }
    // Active reconstruction must change this damaged diagonal; controls are not cosmetic.
    auto raw=source.pixels;raw[3]=128;auto filtered=raw;Reconstruction441 active;active.strongerTangents=30;reconstruct441(filtered,source.width,source.height,3,1,active);check(filtered!=raw);
    {
        auto baseline=raw;artifactCleanup(baseline,source.width,source.height,1,0,2);
        bool toleranceChanges=false,sizeChanges=false;
        for(uint32_t percent:{25u,50u,150u,200u})for(bool tolerance:{false,true}){
            ContourTuning tuning;if(tolerance)tuning.tolerance=percent;else tuning.minimumSize=percent;
            auto a=raw,b=raw;
            artifactCleanup(a,source.width,source.height,1,0,2,256,tuning);
            artifactCleanup(b,source.width,source.height,3,0,2,256,tuning);
            check(a==b&&a[3]==raw[3]);
            if(tolerance)toleranceChanges|=a!=baseline;else sizeChanges|=a!=baseline;
        }
        check(toleranceChanges&&sizeChanges); // Both controls affect actual reconstruction.
    }
    auto single=raw;reconstruct441(single,source.width,source.height,1,1,active);check(single==filtered);check(std::equal(raw.begin(),raw.begin()+4,filtered.begin()));
    {
        auto fixed=active;fixed.fixedIntensity=true;fixed.antialias=fixed.contrast=0;
        auto reference=raw;reconstruct441(reference,source.width,source.height,1,1,fixed);
        check(reference!=raw);
        for(uint32_t budget:{30000u,80000u,200000u}){
            auto actual=raw;reconstruct441(actual,source.width,source.height,3,budget,fixed);
            check(actual==reference); // No hidden density fade in manual mode.
        }
    }
    // High coded density must no longer silently disable the contour control.
    for(uint32_t cap:{30000u,80000u,150000u,300000u}){auto p=raw;reconstruct441(p,source.width,source.height,3,cap,active);check(p!=raw&&std::equal(raw.begin(),raw.begin()+4,p.begin()));
        auto disabled=active;disabled.intensity=disabled.antialias=disabled.contrast=0;auto unchanged=raw;reconstruct441(unchanged,source.width,source.height,3,cap,disabled);check(unchanged==raw);}
    Frame constant{31,17,0,1,Bytes(pixelBytes(31,17),255)};for(size_t i=0;i<constant.pixels.size();i+=4){constant.pixels[i]=65;constant.pixels[i+1]=30;constant.pixels[i+2]=140;}
    auto flat=constant.pixels;reconstruct441(flat,31,17,3,1,active);check(flat==constant.pixels);
    for(auto offset:{0u,5u,6u,20u,22u,24u,26u,28u,30u,40u}){auto bad=packet;bad.data[offset]=255;rejected([&]{codec->decode(bad,source.width,source.height);});}
    auto zeroBudget=packet;std::fill_n(zeroBudget.data.begin()+16,4,0);rejected([&]{codec->decode(zeroBudget,source.width,source.height);});
    auto bad=packet;bad.data.pop_back();rejected([&]{codec->decode(bad,source.width,source.height);});
    bad=packet;bad.data[reconstruction441HeaderBytes+4]=reconstruction441Version;rejected([&]{codec->decode(bad,source.width,source.height);});
    auto invalid=active;invalid.strongerTangents=101;rejected([&]{update441(bad,invalid);});
    invalid=active;invalid.shapes.tolerance=24;rejected([&]{update441(bad,invalid);});
    invalid=active;invalid.shapes.minimumSize=201;rejected([&]{update441(bad,invalid);});
    // Exercise deferred upscale, reused native storage, disabled identity and
    // caller buffers on an image large enough to take the low-rate path.
    Frame scaled{1297,769,17,23,Bytes(pixelBytes(1297,769),255)};
    for(uint32_t y=0;y<scaled.height;++y)for(uint32_t x=0;x<scaled.width;++x){auto* p=scaled.pixels.data()+(size_t(y)*scaled.width+x)*4;
        p[0]=uint8_t(40+(x/11)%160);p[1]=uint8_t(25+(y/9)%170);p[2]=uint8_t(30+(x+y)/17%150);}
    options.targetBytes=6000;options.reconstruction441=Reconstruction441{};
    for(uint32_t contours:{0u,100u}){
        options.shapeIntensity=contours;
        auto coded=codec->encode(scaled,options);auto parsed=read441(coded.data,scaled.width,scaled.height);
        Packet child{Bytes(parsed.child.begin(),parsed.child.end()),17,23};
        DeferredUpscale deferred;auto native=decodeFrameDeferred(child,scaled.width,scaled.height,3,deferred);check(deferred.active);
        auto s=parsed.settings;s.intensity=s.antialias=s.contrast=0;update441(coded,s);
        check(codec->decode(coded,scaled.width,scaled.height).pixels==codec->decode(child,scaled.width,scaled.height).pixels);
        update441(coded,parsed.settings);
        const auto owned=codec->decode(coded,scaled.width,scaled.height);
        Bytes guarded(owned.pixels.size()+32,197);
        decodeFrameToBuffer(coded,scaled.width,scaled.height,1,100,std::span(guarded).subspan(16,owned.pixels.size()),0,true);
        check(std::equal(owned.pixels.begin(),owned.pixels.end(),guarded.begin()+16));
        for(int i=0;i<16;++i)check(guarded[i]==197&&guarded[guarded.size()-1-i]==197);
        check(codec->decode(coded,scaled.width,scaled.height).pixels==owned.pixels);
        if(contours==100){
            const auto unfiltered=decodeFrameWithSettings(coded,scaled.width,scaled.height,1,0,0);
            const auto sharpened=decodeFrameWithSettings(coded,scaled.width,scaled.height,1,100,100);
            check(unfiltered.pixels!=owned.pixels&&sharpened.pixels!=owned.pixels);
            Bytes direct(owned.pixels.size());
            decodeFrameToBuffer(coded,scaled.width,scaled.height,3,0,direct,0);
            check(direct==unfiltered.pixels);
            auto standalone=coded;auto aaSettings=parsed.settings;aaSettings.intensity=0;aaSettings.contrast=0;
            aaSettings.antialias=25;update441(standalone,aaSettings);
            const auto mild=codec->decode(standalone,scaled.width,scaled.height);
            aaSettings.antialias=100;update441(standalone,aaSettings);
            const auto strong=codec->decode(standalone,scaled.width,scaled.height);
            // This smooth synthetic fixture need not contain eligible AA pixels.
            // Verify the standalone path really applies the independent pass;
            // a real compressed scene additionally checks its 1/50/100 range.
            aaSettings.antialias=0;update441(standalone,aaSettings);
            auto expected=codec->decode(standalone,scaled.width,scaled.height);
            aaSettings.antialias=100;
            reconstruct441(expected.pixels,scaled.width,scaled.height,1,parsed.budget,aaSettings);
            check(expected.pixels==strong.pixels&&mild.width==strong.width);
        }
    }
    // Skipping cleanup covered by the final exact-color overlay must preserve
    // the complete output, including the interpolation border next to detail.
    {
        Frame screen{1297,769,17,23,Bytes(pixelBytes(1297,769),255)};
        for(uint32_t y=0;y<screen.height;++y)for(uint32_t x=0;x<screen.width;++x){auto* p=screen.pixels.data()+(size_t(y)*screen.width+x)*4;
            p[0]=30;p[1]=24;p[2]=48;
            if(x>480&&x<800&&y>180&&y<590){p[0]=uint8_t(70+x/7%100);p[1]=uint8_t(45+y/9%90);p[2]=uint8_t(30+(x+y)/11%70);}
        }
        options.targetBytes=12000;options.shapeIntensity=200;options.contourIntensity=0;
        options.reconstruction441=Reconstruction441{};
        const auto coded=codec->encode(screen,options);const auto parsed=read441(coded.data,screen.width,screen.height);
        DeferredUpscale deferred;auto working=decodeFrameDeferred({parsed.child,17,23},screen.width,screen.height,3,deferred);
        check(deferred.active&&!deferred.flatMetadata.empty());
        const auto flat=readFlatRegions(deferred.flatMetadata,screen.width,screen.height);
        for(uint32_t contrast:{0u,35u}){
            auto s=parsed.settings;s.contrast=contrast;auto a=working,b=working;
            auto before=fastReconstruct441(a,screen.width,screen.height,1,parsed.budget,s);
            auto after=fastReconstruct441(b,screen.width,screen.height,3,parsed.budget,s,{},&flat);
            applyFlatRegions(before.pixels,flat,screen.width,screen.height,1);
            applyFlatRegions(after.pixels,flat,screen.width,screen.height,3);
            check(before.pixels==after.pixels);
        }
    }
    std::cout<<"PASS R441: saved controls, source-preserving edits, cap, metadata rejection, recursion rejection, alpha, constant/odd sizes, guarded direct/owned and worker equality, disabled/high-density identity, active visual effect, exact overlay cleanup skip.\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
