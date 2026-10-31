#pragma once
#include "edge_model_avx2.hpp"
#include <array>

// Compact R387-style two-color geometry. Analyze overlapping neighborhoods
// once, then render each original pixel once. No full-frame snapshot is needed.
namespace fajk::sideCleanup {
// Fields 0..23 are the shared R437 guide. 24..26 and 27..29 hold the
// separately confidence-weighted clean side colors; 30/31 their weights.
struct alignas(32) Guide { std::array<float,32> v{}; };
inline float sat(float x) { return std::clamp(x,0.f,1.f); }
inline void orientInPlace(const Guide& reference,Guide& other){
    if(reference.v[0]<.001f||other.v[0]<.001f)return;
    float direction=0;for(int c=0;c<3;++c)direction+=(reference.v[4+c]-reference.v[1+c])*(other.v[4+c]-other.v[1+c]);
    if(direction<0){for(int c=0;c<3;++c)std::swap(other.v[1+c],other.v[4+c]);for(int k=7;k<=9;++k)other.v[k]=-other.v[k];for(int c=0;c<3;++c)std::swap(other.v[24+c],other.v[27+c]);std::swap(other.v[30],other.v[31]);}
}
inline Guide oriented(const Guide& reference,Guide other){orientInPlace(reference,other);return other;}
// Estimate shading outside the contaminated near-edge band. The local
// transition samples locate the shape; they must not define its clean colors.
inline void fitSides(const uint8_t* pixels,uint32_t width,uint32_t height,int cx,int cy,Guide& g,int cleaner=0){
    const float confidence=g.v[0];if(confidence<.03f||g.v[21]>.1f||g.v[20]==0)return;
    // Bright neutral strokes/dividers already have their own R437 contour
    // reconstruction. Remote side colors must not tint or widen them.
    for(int side=0;side<2;++side){float lo=255,hi=0;for(int c=0;c<3;++c){const float value=g.v[1+side*3+c]/confidence;lo=std::min(lo,value);hi=std::max(hi,value);}if(lo>220&&hi-lo<12)return;}
    const float geometry=g.v[20],nx=g.v[7]/geometry,ny=g.v[8]/geometry,d=g.v[9]/geometry+nx*(cx+.5f)+ny*(cy+.5f);
    const float ex=cx-nx*d,ey=cy-ny*d;
    for(int side=0;side<2;++side){
        float xs=0,ys=0,xx=0,xy=0,yy=0;std::array<float,3> sum{},xc{},yc{};float samples[6][3],positions[6][2];int at=0;bool valid=true;
        for(int normal:{cleaner?12:8,cleaner==2?20:cleaner?16:12})for(int tangent:{-4,0,4}){
            const float offset=float(side?normal:-normal);
            // lround-equivalent for the bounded image coordinates, without
            // a CRT call per tap. Double addition preserves float half ties.
            auto coordinate=[](float value){return int(double(value)+(value<0?-.5:.5));};
            const int px=coordinate(ex+nx*offset-ny*tangent),py=coordinate(ey+ny*offset+nx*tangent);
            if(px<0||py<0||px>=int(width)||py>=int(height)){valid=false;continue;}
            const auto* p=pixels+(size_t(py)*width+px)*4;if(p[3]!=255){valid=false;continue;}
            const float x=float(px-cx),y=float(py-cy);positions[at][0]=x;positions[at][1]=y;xs+=x;ys+=y;xx+=x*x;xy+=x*y;yy+=y*y;
            for(int c=0;c<3;++c){samples[at][c]=p[c];sum[c]+=p[c];xc[c]+=x*p[c];yc[c]+=y*p[c];}++at;
        }
        if(!valid||at!=6)continue;
        const float vx=xx-xs*xs/6+2,vy=yy-ys*ys/6+2,cov=xy-xs*ys/6,det=vx*vy-cov*cov;if(det<1)continue;
        std::array<float,3> ax{},ay{},value{};float error=0,difference=0,slope=0;
        for(int c=0;c<3;++c){const float ux=xc[c]-xs*sum[c]/6,uy=yc[c]-ys*sum[c]/6;ax[c]=(ux*vy-uy*cov)/det;ay[c]=(uy*vx-ux*cov)/det;value[c]=(sum[c]-ax[c]*xs-ay[c]*ys)/6;difference=std::max(difference,std::abs(value[c]-g.v[1+side*3+c]/confidence));slope=std::max(slope,std::abs(ax[c])+std::abs(ay[c]));}
        for(int i=0;i<6;++i)for(int c=0;c<3;++c){const float e=samples[i][c]-value[c]-ax[c]*positions[i][0]-ay[c]*positions[i][1];error+=e*e/18;}
        const float errorLimit=cleaner?24.f:16.f,differenceLimit=cleaner?36.f:24.f;
        if(difference>differenceLimit||slope>4||error>errorLimit)continue;
        const float trust=sat((errorLimit-error)/(cleaner?18.f:12.f))*sat((differenceLimit+4-difference)/(cleaner?16.f:12.f));
        g.v[30+side]=trust*confidence;
        for(int c=0;c<3;++c)g.v[24+side*3+c]=value[c]*trust*confidence;
    }
}
inline Guide refined(const uint8_t* pixels,uint32_t width,uint32_t height,int cx,int cy,float sideStrength,bool tolerateRinging,int cleaner=0,ContourTuning tuning={}){
    // Share the exact R437 color/geometry core. Extra cleanup cannot silently
    // change edge placement or diverge when the common analyzer is repaired.
    const auto base=edgeModel::refined(pixels,width,height,cx,cy,tolerateRinging,tuning);
    Guide guide;std::copy(base.v.begin(),base.v.end(),guide.v.begin());
    fitSides(pixels,width,height,cx,cy,guide,cleaner);
    for(int k=24;k<32;++k)guide.v[k]*=sideStrength;
    return guide;
}

template<int grid,bool denoise,int cleaner=0> inline void render(uint8_t* pixels,uint32_t width,uint32_t y,const Guide* guides,uint32_t columns,uint32_t rows,float surfaces,float contours,ContourTuning tuning={}) {
    thread_local std::vector<Guide> vertical;vertical.resize(columns+1);
    const float fy=(float(y)-grid/2)/grid;const int row=int(std::floor(fy));const float ty=fy-row;
    const auto* a=guides+size_t(std::clamp(row,0,int(rows)-1))*columns;
    const auto* b=guides+size_t(std::clamp(row+1,0,int(rows)-1))*columns;
    for(uint32_t i=0;i<columns;++i){const auto other=oriented(a[i],b[i]);for(int k=0;k<32;k+=8)_mm256_store_ps(vertical[i].v.data()+k,_mm256_add_ps(_mm256_mul_ps(_mm256_load_ps(a[i].v.data()+k),_mm256_set1_ps(1-ty)),_mm256_mul_ps(_mm256_load_ps(other.v.data()+k),_mm256_set1_ps(ty))));}
    // Each guide is shared by neighboring cells. Align it once per row rather
    // than copying/reorienting 96 bytes for every rendered SIMD vector.
    for(uint32_t i=1;i<columns;++i)orientInPlace(vertical[i-1],vertical[i]);
    vertical[columns]=vertical[columns-1];
    const auto zero=_mm256_setzero_ps(),one=_mm256_set1_ps(1),half=_mm256_set1_ps(.5f);
    auto clamp=[&](__m256 x){return _mm256_min_ps(one,_mm256_max_ps(zero,x));};
    const auto absMask=_mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
    auto abs=[&](__m256 x){return _mm256_and_ps(x,absMask);};
    // Cell boundaries align with 8 pixels after the four-pixel left margin.
    for(uint32_t x=0;x<width;) {
        const int cell=x<grid/2?-1:int((x-grid/2)/grid);const auto end=std::min(width,uint32_t((cell+1)*grid+grid/2));const auto lanes=std::min(8u,end-x);
        const auto& l=vertical[std::clamp(cell,0,int(columns)-1)].v;const auto& r=vertical[std::clamp(cell+1,0,int(columns)-1)].v;
        if(l[0]+r[0]+surfaces*(l[10]+r[10])<.001f) { x+=lanes;continue; }
        const auto fx=_mm256_mul_ps(_mm256_setr_ps(float(x)-cell*grid-grid/2,float(x+1)-cell*grid-grid/2,float(x+2)-cell*grid-grid/2,float(x+3)-cell*grid-grid/2,float(x+4)-cell*grid-grid/2,float(x+5)-cell*grid-grid/2,float(x+6)-cell*grid-grid/2,float(x+7)-cell*grid-grid/2),_mm256_set1_ps(1.f/grid));
        auto field=[&](int k){return _mm256_add_ps(_mm256_set1_ps(l[k]),_mm256_mul_ps(fx,_mm256_set1_ps(r[k]-l[k])));};
        auto* address=pixels+size_t(x)*4;alignas(32) uint8_t tail[32]{};if(lanes<8)std::memcpy(tail,address,lanes*4);
        const auto packed=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lanes==8?address:tail));
        auto channel=[&](int c){return _mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srlv_epi32(packed,_mm256_set1_epi32(c*8)),_mm256_set1_epi32(255)));};
        __m256 result[3]={channel(0),channel(1),channel(2)};
        const auto px=_mm256_setr_ps(float(x)+.5f,float(x)+1.5f,float(x)+2.5f,float(x)+3.5f,float(x)+4.5f,float(x)+5.5f,float(x)+6.5f,float(x)+7.5f),py=_mm256_set1_ps(float(y)+.5f);
        if(l[0]+r[0]>.001f) {
            const auto confidence=field(0),inverse=_mm256_rcp_ps(_mm256_max_ps(_mm256_set1_ps(.001f),confidence));
            __m256 low[3],delta[3];auto energy=one,t=zero;
            for(int c=0;c<3;++c) { low[c]=_mm256_mul_ps(field(1+c),inverse);delta[c]=_mm256_sub_ps(_mm256_mul_ps(field(4+c),inverse),low[c]);energy=_mm256_add_ps(energy,_mm256_mul_ps(delta[c],delta[c]));t=_mm256_add_ps(t,_mm256_mul_ps(_mm256_sub_ps(result[c],low[c]),delta[c])); }
            t=clamp(_mm256_mul_ps(t,_mm256_rcp_ps(energy)));auto residual=zero;
            for(int c=0;c<3;++c) { const auto d=_mm256_sub_ps(result[c],_mm256_add_ps(low[c],_mm256_mul_ps(t,delta[c])));residual=_mm256_add_ps(residual,_mm256_mul_ps(d,d)); }
            const auto compatible=edgeModel::contourColorCompatibility(delta,residual);
            const auto detailGuard=edgeModel::contourDetailTrust(delta,field(21));
            const auto colorReliability=_mm256_mul_ps(detailGuard,_mm256_mul_ps(confidence,compatible));
            auto amount=zero,phase=zero,orientedRing=zero,skirtRing=zero;
            // Rejected geometry still permits color cleanup, but needs no
            // normals, signed distances, reciprocals or phase reconstruction.
            if(l[20]+r[20]>0){
                const auto geometryInverse=_mm256_rcp_ps(_mm256_max_ps(_mm256_set1_ps(.001f),field(20)));
                const auto nx=_mm256_mul_ps(field(7),geometryInverse),ny=_mm256_mul_ps(field(8),geometryInverse);
                const auto distance=_mm256_add_ps(_mm256_mul_ps(field(9),geometryInverse),_mm256_add_ps(_mm256_mul_ps(px,nx),_mm256_mul_ps(py,ny)));
                const auto desired=clamp(_mm256_add_ps(half,_mm256_mul_ps(distance,_mm256_rcp_ps(_mm256_max_ps(_mm256_set1_ps(.85f),_mm256_max_ps(abs(nx),abs(ny)))))));
                const auto coherence=edgeModel::contourCoherence(nx,ny);
                const auto reliability=_mm256_mul_ps(detailGuard,_mm256_mul_ps(field(20),_mm256_mul_ps(coherence,compatible)));
                // Endpoint-valued pixels can be the staircase itself.
                const auto band=clamp(_mm256_sub_ps(_mm256_set1_ps(2.5f),abs(distance)));
                amount=clamp(_mm256_mul_ps(_mm256_set1_ps(contours),_mm256_mul_ps(reliability,band)));
                const auto ringBand=_mm256_mul_ps(clamp(_mm256_sub_ps(abs(distance),_mm256_set1_ps(1.5f))),clamp(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(cleaner==2?14.f:cleaner?10.f:7.f),abs(distance)),_mm256_set1_ps(cleaner?.125f:.25f))));
                orientedRing=_mm256_mul_ps(coherence,ringBand);
                skirtRing=_mm256_mul_ps(coherence,_mm256_mul_ps(clamp(_mm256_sub_ps(abs(distance),_mm256_set1_ps(1.5f))),clamp(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(14),abs(distance)),_mm256_set1_ps(.125f)))));
                phase=_mm256_min_ps(_mm256_set1_ps(.28f),_mm256_max_ps(_mm256_set1_ps(-.28f),_mm256_sub_ps(desired,t)));
            }
            const auto colorBand=clamp(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(.25f),_mm256_min_ps(t,_mm256_sub_ps(one,t))),_mm256_set1_ps(4)));
            // The extra experiment changes side-color denoising only. Keep
            // bright neutral glyphs/dividers on their historical strength.
            auto extra=zero;
            if constexpr(cleaner){
                auto brightNeutral=[&](__m256 a,__m256 b,__m256 c){const auto lo=_mm256_min_ps(a,_mm256_min_ps(b,c)),hi=_mm256_max_ps(a,_mm256_max_ps(b,c));return _mm256_and_ps(_mm256_cmp_ps(lo,_mm256_set1_ps(220),_CMP_GT_OQ),_mm256_cmp_ps(_mm256_sub_ps(hi,lo),_mm256_set1_ps(12),_CMP_LT_OQ));};
                const auto protectedColor=_mm256_or_ps(brightNeutral(low[0],low[1],low[2]),brightNeutral(_mm256_add_ps(low[0],delta[0]),_mm256_add_ps(low[1],delta[1]),_mm256_add_ps(low[2],delta[2])));
                extra=_mm256_andnot_ps(protectedColor,one);
            }
            const auto cleanupScale=_mm256_add_ps(one,_mm256_mul_ps(extra,_mm256_set1_ps(cleaner==2?1.f:.5f)));
            const auto cleanup=clamp(_mm256_mul_ps(_mm256_mul_ps(_mm256_set1_ps(.8f*surfaces),cleanupScale),_mm256_mul_ps(colorReliability,_mm256_max_ps(colorBand,orientedRing))));
            const auto endpoint=_mm256_and_ps(_mm256_cmp_ps(t,half,_CMP_GE_OQ),one);
            const auto ringingLimit=_mm256_add_ps(_mm256_set1_ps(.15f),_mm256_mul_ps(extra,_mm256_set1_ps(cleaner==2?.15f:.09f)));
            const auto ringing=_mm256_min_ps(ringingLimit,_mm256_max_ps(_mm256_sub_ps(zero,ringingLimit),_mm256_sub_ps(endpoint,t)));
            const auto skirt=edgeModel::contourSkirtWeight(low,delta,colorReliability,skirtRing,std::max(0.f,contours-surfaces));
            const auto phaseCorrection=_mm256_add_ps(_mm256_mul_ps(skirt,_mm256_min_ps(_mm256_set1_ps(.12f),_mm256_max_ps(_mm256_set1_ps(-.12f),phase))),_mm256_add_ps(_mm256_mul_ps(amount,phase),_mm256_mul_ps(cleanup,ringing)));
            const auto noiseWeight=_mm256_add_ps(_mm256_set1_ps(.35f),_mm256_mul_ps(extra,_mm256_set1_ps(cleaner==2?.35f:.20f)));
            const auto noiseCorrection=_mm256_add_ps(_mm256_mul_ps(skirt,_mm256_set1_ps(.5f)),_mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(.18f),amount),_mm256_mul_ps(noiseWeight,cleanup)));
            for(int c=0;c<3;++c) { const auto own=result[c],projected=_mm256_add_ps(low[c],_mm256_mul_ps(t,delta[c]));const auto correction=_mm256_add_ps(_mm256_mul_ps(phaseCorrection,delta[c]),_mm256_mul_ps(noiseCorrection,_mm256_sub_ps(projected,own)));const auto lo=_mm256_min_ps(own,_mm256_min_ps(low[c],_mm256_add_ps(low[c],delta[c]))),hi=_mm256_max_ps(own,_mm256_max_ps(low[c],_mm256_add_ps(low[c],delta[c])));result[c]=_mm256_min_ps(hi,_mm256_max_ps(lo,_mm256_add_ps(own,correction))); }
            if(surfaces&&l[30]+r[30]+l[31]+r[31]>.001f){
                const auto side=_mm256_cmp_ps(t,half,_CMP_GE_OQ);
                const auto trusted=_mm256_blendv_ps(field(30),field(31),side),invTrust=_mm256_rcp_ps(_mm256_max_ps(_mm256_set1_ps(.001f),trusted));
                const auto pure=clamp(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(cleaner?.45f:.30f),_mm256_min_ps(t,_mm256_sub_ps(one,t))),_mm256_set1_ps(1.f/(cleaner?.45f:.30f))));
                const auto cleanAmount=clamp(_mm256_mul_ps(_mm256_set1_ps((cleaner==2?1.25f:cleaner?1.05f:.85f)*surfaces),_mm256_mul_ps(trusted,_mm256_mul_ps(compatible,_mm256_mul_ps(pure,_mm256_mul_ps(orientedRing,detailGuard))))));
                for(int c=0;c<3;++c){const auto target=_mm256_mul_ps(_mm256_blendv_ps(field(24+c),field(27+c),side),invTrust),modelEndpoint=_mm256_add_ps(low[c],_mm256_and_ps(side,delta[c]));const auto correction=_mm256_min_ps(_mm256_set1_ps(cleaner?18.f:12.f),_mm256_max_ps(_mm256_set1_ps(cleaner?-18.f:-12.f),_mm256_sub_ps(target,modelEndpoint)));result[c]=_mm256_add_ps(result[c],_mm256_mul_ps(cleanAmount,correction));if constexpr(denoise){const auto remaining=_mm256_min_ps(_mm256_set1_ps(cleaner?40.f:24.f),_mm256_max_ps(_mm256_set1_ps(cleaner?-40.f:-24.f),_mm256_sub_ps(target,result[c])));result[c]=_mm256_add_ps(result[c],_mm256_mul_ps(_mm256_mul_ps(cleanAmount,_mm256_set1_ps(cleaner==2?.8f:cleaner?.65f:.45f)),remaining));}}
            }
        }
        if(surfaces&&l[10]+r[10]>.001f) {
            const auto confidence=field(10),inverse=_mm256_rcp_ps(_mm256_max_ps(_mm256_set1_ps(.001f),confidence));
            __m256 planar[3];auto distance=zero;
            if(l[22]+r[22]==0){for(int c=0;c<3;++c){planar[c]=_mm256_mul_ps(inverse,field(11+c));distance=_mm256_max_ps(distance,abs(_mm256_sub_ps(planar[c],channel(c))));}}
            else for(int c=0;c<3;++c) { planar[c]=_mm256_mul_ps(inverse,_mm256_add_ps(field(11+c),_mm256_add_ps(_mm256_mul_ps(px,field(14+c)),_mm256_mul_ps(py,field(17+c)))));distance=_mm256_max_ps(distance,abs(_mm256_sub_ps(planar[c],channel(c)))); }
            // With no edge correction and a planar delta below half an
            // output level, the clamped blend cannot change any rounded byte.
            // Skip packing/storing these already clean SIMD spans exactly.
            if(l[0]+r[0]<=.001f&&_mm256_movemask_ps(_mm256_cmp_ps(distance,_mm256_set1_ps(.49f),_CMP_LT_OQ))==255){x+=lanes;continue;}
            const auto amount=clamp(_mm256_mul_ps(_mm256_set1_ps(.55f*surfaces),_mm256_mul_ps(confidence,clamp(_mm256_sub_ps(one,_mm256_mul_ps(distance,_mm256_set1_ps(1.f/28)))))));
            for(int c=0;c<3;++c)result[c]=_mm256_add_ps(result[c],_mm256_mul_ps(amount,_mm256_sub_ps(planar[c],result[c])));
        }
        __m256i values[3];for(int c=0;c<3;++c)values[c]=_mm256_cvttps_epi32(_mm256_add_ps(_mm256_min_ps(_mm256_set1_ps(255),_mm256_max_ps(zero,result[c])),half));
        const auto output=_mm256_or_si256(_mm256_and_si256(packed,_mm256_set1_epi32(int(0xff000000u))),_mm256_or_si256(values[0],_mm256_or_si256(_mm256_slli_epi32(values[1],8),_mm256_slli_epi32(values[2],16))));
        const auto final=_mm256_blendv_epi8(packed,output,_mm256_cmpeq_epi32(_mm256_srli_epi32(packed,24),_mm256_set1_epi32(255)));
        if(lanes==8)_mm256_storeu_si256(reinterpret_cast<__m256i*>(address),final);else { _mm256_store_si256(reinterpret_cast<__m256i*>(tail),final);std::memcpy(address,tail,lanes*4); }
        x+=lanes;
    }
    _mm256_zeroupper();
}
template<int grid,bool denoise,int cleaner=0> inline void restoreGrid(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,float surfaces,float contours,ContourTuning tuning={}) {
    if(!surfaces&&!contours)return;
    TimingTimeline timing;const auto columns=(width+grid-1)/grid,rows=(height+grid-1)/grid;
    thread_local std::vector<Guide> cache;cache.resize(size_t(columns)*rows);auto& guides=cache;
    const float sideStrength=sat((surfaces-.1f)*2.5f);
    parallelFor(rows,workers,[&](uint32_t y){for(uint32_t x=0;x<columns;++x)guides[size_t(y)*columns+x]=refined(pixels.data(),width,height,int(x*grid+grid/2),int(y*grid+grid/2),sideStrength,denoise,cleaner,tuning);});
    // Isolated low-contrast plateaus need spatial context, not an edge label.
    // Fit the surrounding guide planes while excluding the suspect center.
    // A real boundary fails the surrounding affine-consistency test.
    if(surfaces>.35f&&columns>=7&&rows>=7){
        thread_local std::vector<Guide> original;original=guides;const auto& snapshot=original;
        parallelFor(rows-6,workers,[&](uint32_t job){const auto gy=job+3;for(uint32_t gx=3;gx+3<columns;++gx){
            auto& out=guides[size_t(gy)*columns+gx];const auto& own=snapshot[size_t(gy)*columns+gx];
            if(own.v[0]>.03f||own.v[10]<.65f)continue;
            const float cx=float(gx*grid+grid/2)+.5f,cy=float(gy*grid+grid/2)+.5f;
            float weight=0;std::array<float,3> center{},sx{},sy{};bool valid=true;
            for(int dy=-1;dy<=1&&valid;++dy)for(int dx=-1;dx<=1&&valid;++dx){if(!dx&&!dy)continue;const auto& g=snapshot[size_t(int(gy)+dy*3)*columns+int(gx)+dx*3];
                if(g.v[0]>.03f||g.v[10]<.65f){valid=false;continue;}
                weight+=g.v[10];
                if(g.v[22]==0){for(int c=0;c<3;++c)center[c]+=g.v[11+c];}
                else for(int c=0;c<3;++c){center[c]+=g.v[11+c]+cx*g.v[14+c]+cy*g.v[17+c];sx[c]+=g.v[14+c];sy[c]+=g.v[17+c];}
            }
            if(!valid||weight<5.2f)continue;
            for(int c=0;c<3;++c){center[c]/=weight;sx[c]/=weight;sy[c]/=weight;}
            float deviation=0,spread=0,minimumDifference=1000,maximumDifference=-1000;
            for(int c=0;c<3;++c){const float difference=center[c]-(own.v[11+c]+cx*own.v[14+c]+cy*own.v[17+c])/own.v[10];deviation=std::max(deviation,std::abs(difference));minimumDifference=std::min(minimumDifference,difference);maximumDifference=std::max(maximumDifference,difference);}
            // A shared lightness offset is stronger evidence of a quantized
            // plateau than a different legitimate color or gentle curvature.
            if(deviation<=3||deviation>=16||maximumDifference-minimumDifference>4)continue;
            for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){if(!dx&&!dy)continue;const auto& g=snapshot[size_t(int(gy)+dy*3)*columns+int(gx)+dx*3];for(int c=0;c<3;++c){const float d=(g.v[11+c]+cx*g.v[14+c]+cy*g.v[17+c])/g.v[10]-center[c];spread+=d*d/24;}}
            if(spread>4)continue;
            const float blend=.75f*sat((4-spread)/4)*sat((surfaces-.35f)*4)*sat((deviation-3)*.5f)*sat((16-deviation)*.25f);
            for(int c=0;c<3;++c){out.v[11+c]+=(own.v[10]*(center[c]-cx*sx[c]-cy*sy[c])-out.v[11+c])*blend;out.v[14+c]+=(own.v[10]*sx[c]-out.v[14+c])*blend;out.v[17+c]+=(own.v[10]*sy[c]-out.v[17+c])*blend;}
            out.v[22]=1;
        }});
    }
    timing.mark("decode.unified385.guide");
    parallelFor((height+15)/16,workers,[&](uint32_t job){for(uint32_t y=job*16;y<std::min(height,(job+1)*16);++y)render<grid,denoise,cleaner>(pixels.data()+size_t(y)*width*4,width,y,guides.data(),columns,rows,surfaces,contours);});timing.mark("decode.unified385.render");
}
inline void restore(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,float surfaces,float contours,bool denoise=false,int cleaner=0,ContourTuning tuning={}){
    // At zero extra strength the R437 core is exactly the same renderer.
    // Avoid building wider guides and fitting remote colors for a zero delta.
    if(surfaces<=.1f){edgeModel::restore(pixels,width,height,workers,surfaces,contours,denoise,tuning);return;}
    // Weak corrections need fewer overlapping guides, not another algorithm.
    if(cleaner==1){restoreGrid<8,true,1>(pixels,width,height,workers,surfaces,contours,tuning);return;}
    if(cleaner==2){restoreGrid<8,true,2>(pixels,width,height,workers,surfaces,contours,tuning);return;}
    if(denoise){if(surfaces<.20f&&contours<.45f)restoreGrid<16,true>(pixels,width,height,workers,surfaces,contours,tuning);else restoreGrid<8,true>(pixels,width,height,workers,surfaces,contours,tuning);}
    else {if(surfaces<.20f&&contours<.45f)restoreGrid<16,false>(pixels,width,height,workers,surfaces,contours,tuning);else restoreGrid<8,false>(pixels,width,height,workers,surfaces,contours,tuning);}
}
}
