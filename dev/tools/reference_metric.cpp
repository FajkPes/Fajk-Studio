#include "fajk/media.hpp"
#include "lib/jxl/image_bundle.h"
#include "lib/jxl/color_encoding_internal.h"
#include "tools/ssimulacra2.h"
#include "tools/no_memory_manager.h"
#include <cstdio>
#include <cstdlib>

int wmain(int argc,wchar_t** argv){
    if(argc<3)return 1;
    try{
        jxl::ImageMetadata metadata;fajk::require(bool(metadata.color_encoding.SetSRGB(jxl::ColorSpace::kRGB)),"sRGB metadata failed.");
        auto load=[&](const wchar_t* path){fajk::MediaSource source(path);fajk::Frame f;fajk::require(source.next(f),"No image.");auto created=jxl::Image3F::Create(jpegxl::tools::NoMemoryManager(),f.width,f.height);fajk::require(bool(created.status()),"Metric allocation failed.");auto image=std::move(created).value_();
            for(uint32_t y=0;y<f.height;++y)for(int c=0;c<3;++c){auto* row=image.PlaneRow(c,y);for(uint32_t x=0;x<f.width;++x)row[x]=f.pixels[(size_t(y)*f.width+x)*4+c]/255.f;}
            jxl::ImageBundle bundle(jpegxl::tools::NoMemoryManager(),&metadata);fajk::require(bool(bundle.SetFromImage(std::move(image),metadata.color_encoding)),"Metric image failed.");return bundle;
        };
        auto original=load(argv[1]);
        for(int i=2;i<argc;++i){auto reconstructed=load(argv[i]);fajk::require(original.xsize()==reconstructed.xsize()&&original.ysize()==reconstructed.ysize(),"Metric dimensions differ.");auto score=ComputeSSIMULACRA2(original,reconstructed);fajk::require(bool(score.status()),"Metric failed.");auto result=std::move(score).value_();std::printf("%.9f\n",result.Score());if(std::getenv("FAJK_METRIC_COMPONENTS")){for(size_t scale=0;scale<result.scales.size();++scale)for(int c=0;c<3;++c)for(int n=0;n<2;++n)std::printf("component,%zu,%d,%d,%.12g,%.12g,%.12g\n",scale,c,n,result.scales[scale].avg_ssim[c*2+n],result.scales[scale].avg_edgediff[c*4+n],result.scales[scale].avg_edgediff[c*4+2+n]);}}
        return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 2;}
}
