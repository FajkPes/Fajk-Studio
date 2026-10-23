#include "artifact_cleanup.hpp"
#include <immintrin.h>
#include "side_cleanup_avx2.hpp"
#include <ostream>
namespace fajk {
template<int grid,bool recovery> Frame effectMap(const Frame& input,float contours){
    auto frame=input;const auto columns=(input.width+grid-1)/grid,rows=(input.height+grid-1)/grid;
    std::vector<edgeModel::Guide> guides;
    edgeModel::buildGuides<grid,recovery>(input.pixels.data(),input.width,input.height,0,0,contours,true,guides);
    for(uint32_t y=0;y<input.height;++y)edgeModel::render<grid,false,true,!recovery>(frame.pixels.data()+size_t(y)*input.width*4,input.width,y,guides.data(),columns,rows,0,contours);
    return frame;
}
Frame contourEffectMap(const Frame& frame,uint32_t grid,float contours,bool recovery){
    frame.validate();require(hasAvx2()&&(grid==6||grid==8||grid==16)&&std::isfinite(contours)&&contours>=0&&contours<=2,"Invalid contour effect diagnostic.");
    if(grid==6)return recovery?effectMap<6,true>(frame,contours):effectMap<6,false>(frame,contours);
    if(grid==16)return recovery?effectMap<16,true>(frame,contours):effectMap<16,false>(frame,contours);
    return recovery?effectMap<8,true>(frame,contours):effectMap<8,false>(frame,contours);
}
void reportContourGuides(const Frame& frame,uint32_t grid,std::ostream& out,bool recovery,float contours){
    frame.validate();require(hasAvx2()&&(grid==6||grid==8||grid==16)&&std::isfinite(contours)&&contours>=0&&contours<=2,"Invalid contour diagnostic grid.");
    out<<"x,y,broad_color,broad_geometry,fine_color,fine_geometry,color,geometry,thin,broad_reason,fine_reason,broad_span,fine_span,broad_energy,fine_energy,broad_color_error,fine_color_error,broad_plane_error,fine_plane_error,broad_located,fine_located,broad_location_reason,fine_location_reason,broad_crossings,fine_crossings,broad_location_error,fine_location_error\n";
    const auto columns=(frame.width+grid-1)/grid,rows=(frame.height+grid-1)/grid;
    std::vector<edgeModel::Guide> guides;
    auto build=[&]<int spacing>(){if(recovery)edgeModel::buildGuides<spacing,true>(frame.pixels.data(),frame.width,frame.height,0,0,contours,true,guides);else edgeModel::buildGuides<spacing,false>(frame.pixels.data(),frame.width,frame.height,0,0,contours,true,guides);};
    if(grid==6)build.template operator()<6>();else if(grid==8)build.template operator()<8>();else build.template operator()<16>();
    for(uint32_t row=0;row<rows;++row)for(uint32_t column=0;column<columns;++column){
        const auto x=column*grid+grid/2,y=row*grid+grid/2;
        edgeModel::AnalysisDebug bd,fd;edgeModel::LocationDebug bl,fl;
        const auto broad=edgeModel::analyze<true>(frame.pixels.data(),frame.width,frame.height,x,y,4,true,&bd);
        const auto fine=edgeModel::analyze<true>(frame.pixels.data(),frame.width,frame.height,x,y,2,true,&fd);
        const auto locatedBroad=edgeModel::locateCrossings<true>(frame.pixels.data(),frame.width,frame.height,x,y,broad,true,&bl);
        const auto locatedFine=edgeModel::locateCrossings<true>(frame.pixels.data(),frame.width,frame.height,x,y,fine,true,&fl);
        const auto& guide=guides[size_t(row)*columns+column];
        out<<x<<','<<y<<','<<broad.v[0]<<','<<broad.v[20]<<','<<fine.v[0]<<','<<fine.v[20]<<','<<guide.v[0]<<','<<guide.v[20]<<','<<guide.v[21]
           <<','<<bd.reason<<','<<fd.reason<<','<<bd.span<<','<<fd.span<<','<<bd.energy<<','<<fd.energy<<','<<bd.colorError<<','<<fd.colorError<<','<<bd.planeError<<','<<fd.planeError
           <<','<<locatedBroad.v[20]<<','<<locatedFine.v[20]<<','<<bl.reason<<','<<fl.reason<<','<<bl.count<<','<<fl.count<<','<<bl.error<<','<<fl.error<<'\n';
    }
}
void artifactCleanupAvx2(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,float surfaces,float contours,uint32_t spacing){
    require(spacing==256,"Only the current artifact reconstruction is supported.");
    sideCleanup::restore(pixels,width,height,workers,surfaces,contours,true);
}
}
