#pragma once
#include <array>
#include "contour_grid.hpp"

// Compact R387-style two-color geometry. Analyze overlapping neighborhoods
// once, then render each original pixel once. No full-frame snapshot is needed.
namespace fajk::edgeModel {
struct alignas(32) Guide { std::array<float,24> v{}; };
// Collected only by the explicit developer inspection command. Normal decode
// instantiates the diagnostic=false specializations with no counters or IO.
struct AnalysisDebug {int reason=0;float span=0,energy=0,colorError=0,planeError=0;};
struct LocationDebug {int reason=0,count=0;float error=0;};
inline float sat(float x) { return std::clamp(x,0.f,1.f); }
inline float sum8(__m256 value) { auto v=_mm_add_ps(_mm256_castps256_ps128(value),_mm256_extractf128_ps(value,1));v=_mm_hadd_ps(v,v);v=_mm_hadd_ps(v,v);return _mm_cvtss_f32(v); }
inline float minimum8(__m256 value){auto v=_mm_min_ps(_mm256_castps256_ps128(value),_mm256_extractf128_ps(value,1));v=_mm_min_ps(v,_mm_movehl_ps(v,v));return _mm_cvtss_f32(_mm_min_ss(v,_mm_shuffle_ps(v,v,1)));}
inline float maximum8(__m256 value){auto v=_mm_max_ps(_mm256_castps256_ps128(value),_mm256_extractf128_ps(value,1));v=_mm_max_ps(v,_mm_movehl_ps(v,v));return _mm_cvtss_f32(_mm_max_ss(v,_mm_shuffle_ps(v,v,1)));}
template<bool diagnostic=false> inline Guide analyze(const uint8_t* pixels,uint32_t width,uint32_t height,int cx,int cy,int step=4,bool tolerant=false,AnalysisDebug* debug=nullptr) {
    if constexpr(diagnostic)*debug={};
    alignas(32) float sample[3][16];
    const float scale=step*.25f;
    const auto wx=_mm256_mul_ps(_mm256_setr_ps(-6,-2,2,6,-6,-2,2,6),_mm256_set1_ps(scale));
    const __m256 wy[2]={_mm256_mul_ps(_mm256_setr_ps(-6,-6,-6,-6,-2,-2,-2,-2),_mm256_set1_ps(scale)),_mm256_mul_ps(_mm256_setr_ps(2,2,2,2,6,6,6,6),_mm256_set1_ps(scale))};
    uint32_t uniformColor=0;bool uniform=true;
    for(int half=0;half<2;++half) {
        const auto xs=_mm256_min_epi32(_mm256_set1_epi32(width-1),_mm256_max_epi32(_mm256_setzero_si256(),
            _mm256_add_epi32(_mm256_set1_epi32(cx-step*3/2),_mm256_mullo_epi32(_mm256_setr_epi32(0,1,2,3,0,1,2,3),_mm256_set1_epi32(step)))));
        const auto ys=_mm256_min_epi32(_mm256_set1_epi32(height-1),_mm256_max_epi32(_mm256_setzero_si256(),
            _mm256_add_epi32(_mm256_set1_epi32(cy+(half*2)*step-step*3/2),_mm256_mullo_epi32(_mm256_setr_epi32(0,0,0,0,1,1,1,1),_mm256_set1_epi32(step)))));
        const auto indices=_mm256_add_epi32(xs,_mm256_mullo_epi32(ys,_mm256_set1_epi32(width)));
        const auto rgba=_mm256_i32gather_epi32(reinterpret_cast<const int*>(pixels),indices,4);
        if(_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_srli_epi32(rgba,24),_mm256_set1_epi32(255))))!=255){if constexpr(diagnostic)debug->reason=1;return {};}
        if(!half)uniformColor=uint32_t(_mm256_extract_epi32(rgba,0));
        uniform=uniform&&_mm256_movemask_epi8(_mm256_cmpeq_epi32(rgba,_mm256_set1_epi32(int(uniformColor))))==-1;
        for(int c=0;c<3;++c)_mm256_store_ps(sample[c]+half*8,_mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srlv_epi32(rgba,_mm256_set1_epi32(c*8)),_mm256_set1_epi32(255))));
    }
    // Constant opaque taps have exactly zero least-squares slope/error. The
    // dense-center probe in refined() still checks details between these taps.
    if(uniform){if constexpr(diagnostic)debug->reason=2;Guide guide;guide.v[10]=1;for(int c=0;c<3;++c)guide.v[11+c]=float((uniformColor>>(c*8))&255);return guide;}
    std::array<float,3> lo{},hi{},mean{},sx{},sy{};float planeError=0;
    for(int c=0;c<3;++c) {
        const auto v0=_mm256_load_ps(sample[c]),v1=_mm256_load_ps(sample[c]+8),sum=_mm256_add_ps(v0,v1);
        mean[c]=sum8(sum)/16;sx[c]=sum8(_mm256_mul_ps(sum,wx))/(320*scale*scale);sy[c]=sum8(_mm256_add_ps(_mm256_mul_ps(v0,wy[0]),_mm256_mul_ps(v1,wy[1])))/(320*scale*scale);
        lo[c]=minimum8(_mm256_min_ps(v0,v1));hi[c]=maximum8(_mm256_max_ps(v0,v1));
        for(int h=0;h<2;++h) { const auto expected=_mm256_add_ps(_mm256_set1_ps(mean[c]),_mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(sx[c]),wx),_mm256_mul_ps(_mm256_set1_ps(sy[c]),wy[h])));const auto d=_mm256_sub_ps(_mm256_load_ps(sample[c]+h*8),expected);planeError+=sum8(_mm256_mul_ps(d,d))/48; }
    }
    Guide guide;guide.v[10]=sat((64-planeError)/56);
    for(int c=0;c<3;++c)if(sx[c]!=0||sy[c]!=0)guide.v[22]=1;
    for(int c=0;c<3;++c) { guide.v[11+c]=(mean[c]-sx[c]*(cx+.5f)-sy[c]*(cy+.5f))*guide.v[10];guide.v[14+c]=sx[c]*guide.v[10];guide.v[17+c]=sy[c]*guide.v[10]; }
    const float noiseFloor=tolerant?1.5f:3.f;
    if constexpr(diagnostic)debug->planeError=planeError;
    if(planeError<noiseFloor){if constexpr(diagnostic)debug->reason=3;return guide;}
    int channel=0;for(int c=1;c<3;++c)if(hi[c]-lo[c]>hi[channel]-lo[channel])channel=c;
    if constexpr(diagnostic)debug->span=hi[channel]-lo[channel];
    if(hi[channel]-lo[channel]<(tolerant?16:24)){if constexpr(diagnostic)debug->reason=4;return guide;}
    std::array<float,3> a{},b{};const auto middle=_mm256_set1_ps((hi[channel]+lo[channel])*.5f);auto count=_mm256_setzero_ps();__m256 mask[2];
    for(int h=0;h<2;++h) { mask[h]=_mm256_cmp_ps(_mm256_load_ps(sample[channel]+h*8),middle,_CMP_GT_OQ);count=_mm256_add_ps(count,_mm256_and_ps(mask[h],_mm256_set1_ps(1))); }
    const float nb=sum8(count),na=16-nb;if(na<2||nb<2){if constexpr(diagnostic)debug->reason=5;return guide;}
    for(int c=0;c<3;++c) { const auto v0=_mm256_load_ps(sample[c]),v1=_mm256_load_ps(sample[c]+8);a[c]=sum8(_mm256_add_ps(_mm256_andnot_ps(mask[0],v0),_mm256_andnot_ps(mask[1],v1)))/na;b[c]=sum8(_mm256_add_ps(_mm256_and_ps(mask[0],v0),_mm256_and_ps(mask[1],v1)))/nb; }
    if(a>b)std::swap(a,b);
    const float minimumEnergy=tolerant?196.f:400.f;
    float energy=0;std::array<float,3> delta{};for(int c=0;c<3;++c) { delta[c]=b[c]-a[c];energy+=delta[c]*delta[c]; }
    if constexpr(diagnostic)debug->energy=energy;
    if(energy<minimumEnergy){if constexpr(diagnostic)debug->reason=6;return guide;}
    alignas(32) std::array<float,16> alpha{};float gx=0,gy=0,error=0;
    for(int h=0;h<2;++h) { auto t=_mm256_setzero_ps();for(int c=0;c<3;++c)t=_mm256_add_ps(t,_mm256_mul_ps(_mm256_sub_ps(_mm256_load_ps(sample[c]+h*8),_mm256_set1_ps(a[c])),_mm256_set1_ps(delta[c])));t=_mm256_min_ps(_mm256_set1_ps(1),_mm256_max_ps(_mm256_setzero_ps(),_mm256_mul_ps(t,_mm256_set1_ps(1/energy))));_mm256_store_ps(alpha.data()+h*8,t);gx+=sum8(_mm256_mul_ps(t,wx));gy+=sum8(_mm256_mul_ps(t,wy[h]));for(int c=0;c<3;++c) { const auto d=_mm256_sub_ps(_mm256_load_ps(sample[c]+h*8),_mm256_add_ps(_mm256_set1_ps(a[c]),_mm256_mul_ps(t,_mm256_set1_ps(delta[c]))));error+=sum8(_mm256_mul_ps(d,d))/48; } }
    if constexpr(diagnostic)debug->colorError=error;
    const float norm=std::sqrt(gx*gx+gy*gy);if(norm<.01f){if constexpr(diagnostic)debug->reason=7;return guide;}
    const float nx=gx/norm,ny=gy/norm;float offsets=0,squares=0;int crossings=0;
    for(int y=0;y<4;++y)for(int x=0;x<4;++x)for(int axis=0;axis<2;++axis) {
        if((!axis&&x==3)||(axis&&y==3))continue;
        const float p=alpha[y*4+x],q=alpha[y*4+x+(axis?4:1)];if((p<.5f)==(q<.5f))continue;
        const float t=sat((.5f-p)/(q-p));const float d=nx*(x*4-6+(axis?0:4*t))*scale+ny*(y*4-6+(axis?4*t:0))*scale;offsets+=d;squares+=d*d;++crossings;
    }
    const float offset=crossings?offsets/crossings:0,variance=crossings?std::max(0.f,squares/crossings-offset*offset):16;int wrong=0;
    for(int i=0;i<16;++i)wrong+=((nx*(i%4*4-6)*scale+ny*(i/4*4-6)*scale-offset)>=0)!=(alpha[i]>=.5f);
    // Color compatibility and geometric confidence answer different questions.
    // A noisy silhouette can still have reliable endpoint colors.
    const float chromatic=sat((*std::max_element(delta.begin(),delta.end())-*std::min_element(delta.begin(),delta.end())-32)/64);
    // Colored silhouettes commonly contain shading and compression spill.
    // Allow that residual without relaxing neutral text/stroke protection.
    const float tolerance=.02f+.025f*chromatic;
    const float allowance=16+energy*tolerance;
    const float fit=std::max(sat((allowance-error)/(12+energy*(tolerance-.005f))),chromatic*.4f*allowance/(allowance+error));
    const float confidence=sat((energy-minimumEnergy)/(tolerant?900.f:1200.f))*fit*sat((planeError-noiseFloor)/25);
    guide.v[20]=confidence*(crossings>=2?sat((5.f-wrong)/3)*sat((16-variance)/12):0);
    guide.v[0]=confidence;
    for(int c=0;c<3;++c) { guide.v[1+c]=a[c]*confidence;guide.v[4+c]=b[c]*confidence; }
    guide.v[7]=nx*confidence;guide.v[8]=ny*confidence;
    guide.v[9]=(-nx*(cx+.5f)-ny*(cy+.5f)-offset)*confidence;
    return guide;
}
inline void orientInPlace(const Guide& reference,Guide& other){
    if(reference.v[0]<.001f||other.v[0]<.001f)return;
    float direction=0;for(int c=0;c<3;++c)direction+=(reference.v[4+c]-reference.v[1+c])*(other.v[4+c]-other.v[1+c]);
    if(direction<0){for(int c=0;c<3;++c)std::swap(other.v[1+c],other.v[4+c]);for(int k=7;k<=9;++k)other.v[k]=-other.v[k];}
}
inline Guide oriented(const Guide& reference,Guide other){orientInPlace(reference,other);return other;}
inline Guide contourOnly(Guide g){
    // Endpoints and normals must come from the same located boundaries.
    // Unlocated color models cannot tint a neighbor's valid contour, and an
    // unlocated thin-stroke flag must not suppress its geometry either.
    const float ratio=g.v[0]>.001f?g.v[20]/g.v[0]:0;
    for(int c=1;c<7;++c)g.v[c]*=ratio;
    g.v[0]=g.v[20];g.v[21]*=g.v[20];
    return g;
}
template<bool diagnostic=false> inline Guide locateCrossings(const uint8_t* pixels,uint32_t width,uint32_t height,int cx,int cy,Guide guide,bool tolerateRinging=false,LocationDebug* debug=nullptr){
    if constexpr(diagnostic)*debug={};
    if(guide.v[0]<.03f){if constexpr(diagnostic)debug->reason=1;return guide;}
    const float confidence=guide.v[0],nx=guide.v[7]/confidence,ny=guide.v[8]/confidence,intercept=guide.v[9]/confidence;
    const bool horizontal=std::abs(nx)>=std::abs(ny);const float normal=horizontal?nx:ny;
    if(std::abs(normal)<.5f){if constexpr(diagnostic)debug->reason=2;return guide;}
    int channel=0;for(int c=1;c<3;++c)if(std::abs(guide.v[4+c]-guide.v[1+c])>std::abs(guide.v[4+channel]-guide.v[1+channel]))channel=c;
    const float low=guide.v[1+channel]/confidence,delta=guide.v[4+channel]/confidence-low;
    if(std::abs(delta)<(tolerateRinging?12.f:20.f)){if constexpr(diagnostic)debug->reason=3;return guide;}
    // RGB compatibility was established by the model. Locate crossings in its
    // strongest channel, with one reciprocal per guide rather than per tap.
    const float inverseDelta=1/delta,inverseNormal=1/normal;
    auto phase=[&](int x,int y){return (pixels[(size_t(y)*width+x)*4+channel]-low)*inverseDelta;};
    float along[5],cross[5];int count=0;bool thin=false;
    for(int line=-4;line<=4;line+=2){const int fixed=(horizontal?cy:cx)+line;if(fixed<0||fixed>=int(horizontal?height:width))continue;
        const float predicted=-(intercept+(horizontal?ny:nx)*(fixed+.5f))*inverseNormal;
        const int first=int(std::floor(predicted-.5f))-2;int hits=0;float crossing=0;
        for(int k=0;k<6;++k){const int at=first+k;if(at<0||at+1>=int(horizontal?width:height))continue;
            const float a=horizontal?phase(at,fixed):phase(fixed,at),b=horizontal?phase(at+1,fixed):phase(fixed,at+1);
            if((a<.5f)==(b<.5f)||std::abs(a-b)<.05f)continue;
            crossing=at+.5f+sat((.5f-a)/(b-a));++hits;
        }
        // R439: shallow oscillations around one net transition are ringing,
        // not a protected text stroke. Genuine strokes reverse deeply or
        // return to the starting side and retain the original guard.
        if(tolerateRinging&&hits>1&&first>=0&&first+6<int(horizontal?width:height)){
            float values[7];for(int k=0;k<7;++k)values[k]=horizontal?phase(first+k,fixed):phase(fixed,first+k);
            const float change=values[6]-values[0],direction=change<0?-1.f:1.f;
            float peak=values[0]*direction,reverse=0;
            for(int k=1;k<7;++k){const float value=values[k]*direction;reverse=std::max(reverse,peak-value);peak=std::max(peak,value);}
            if(std::abs(change)>.65f&&reverse<.35f){
                float signedCrossings=0;int balance=0;
                for(int k=0;k<6;++k){const float a=values[k],b=values[k+1];if((a<.5f)==(b<.5f)||std::abs(a-b)<.05f)continue;const int sign=(b-a)*direction>0?1:-1;signedCrossings+=sign*(first+k+.5f+sat((.5f-a)/(b-a)));balance+=sign;}
                if(balance==1){crossing=signedCrossings;hits=1;}
            }
        }
        // Remaining multiple crossings describe a thin stroke or texture.
        if(hits>1)thin=true;
        if(hits==1){along[count]=float(line);cross[count]=crossing; ++count;}
    }
    guide.v[21]=thin?1.f:0.f;
    float colorSpread=0,minDelta=255,maxDelta=-255;
    for(int c=0;c<3;++c){const float delta=(guide.v[4+c]-guide.v[1+c])/confidence;minDelta=std::min(minDelta,delta);maxDelta=std::max(maxDelta,delta);}colorSpread=maxDelta-minDelta;
    const bool partial=tolerateRinging&&count==2&&colorSpread>48&&std::abs(along[1]-along[0])>=4;
    if constexpr(diagnostic)debug->count=count;
    if(count<3&&!partial){if constexpr(diagnostic)debug->reason=4;guide.v[20]=0;return guide;}
    float sumT=0,sumP=0,sumTT=0,sumTP=0;for(int i=0;i<count;++i){sumT+=along[i];sumP+=cross[i];sumTT+=along[i]*along[i];sumTP+=along[i]*cross[i];}
    const float denominator=count*sumTT-sumT*sumT;if(denominator<=0)return guide;
    const float slope=(count*sumTP-sumT*sumP)/denominator,center=(sumP-slope*sumT)/count;
    if(std::abs(slope)>1.5f){if constexpr(diagnostic)debug->reason=5;guide.v[20]=0;return guide;}
    float error=0;for(int i=0;i<count;++i){const float d=cross[i]-center-slope*along[i];error+=d*d/count;}
    const float sign=normal<0?-1.f:1.f,scale=sign/std::sqrt(1+slope*slope);
    const float newNx=horizontal?scale:-slope*scale,newNy=horizontal?-slope*scale:scale;
    const float newIntercept=horizontal?-newNx*center-newNy*(cy+.5f):-newNy*center-newNx*(cx+.5f);
    guide.v[7]=newNx*confidence;guide.v[8]=newNy*confidence;guide.v[9]=newIntercept*confidence;
    const float tolerance=tolerateRinging?1.5f+1.5f*sat((colorSpread-48)/64):1.5f;
    if constexpr(diagnostic){debug->error=error;if(error>=tolerance)debug->reason=6;}
    guide.v[20]=confidence*sat((tolerance-error)/tolerance)*(partial?.65f:1.f);
    return guide;
}
template<bool coherent> inline Guide locateContour(const uint8_t* pixels,uint32_t width,uint32_t height,int cx,int cy,Guide guide,bool tolerant){
    if constexpr(!coherent)return locateCrossings(pixels,width,height,cx,cy,guide,tolerant);
    const float confidence=guide.v[0];
    if(!tolerant||confidence<.03f)return locateCrossings(pixels,width,height,cx,cy,guide,tolerant);
    std::array<float,3> low{},delta{};float energy=0,minDelta=255,maxDelta=-255;
    for(int c=0;c<3;++c){low[c]=guide.v[1+c]/confidence;delta[c]=guide.v[4+c]/confidence-low[c];energy+=delta[c]*delta[c];minDelta=std::min(minDelta,delta[c]);maxDelta=std::max(maxDelta,delta[c]);}
    if(maxDelta-minDelta<48)return locateCrossings(pixels,width,height,cx,cy,guide,tolerant);
    const float nx=guide.v[7]/confidence,ny=guide.v[8]/confidence,intercept=guide.v[9]/confidence;
    const bool horizontal=std::abs(nx)>=std::abs(ny);const float normal=horizontal?nx:ny;
    if(std::abs(normal)<.5f)return locateCrossings(pixels,width,height,cx,cy,guide,tolerant);
    const float inverseEnergy=1/std::max(1.f,energy),inverseNormal=1/normal;
    float along[9],cross[9];int count=0;
    for(int line=-8;line<=8;line+=4){
        const int fixed=(horizontal?cy:cx)+line;if(fixed<0||fixed>=int(horizontal?height:width))continue;
        const float predicted=-(intercept+(horizontal?ny:nx)*(fixed+.5f))*inverseNormal;
        const int first=int(std::floor(predicted-.5f))-4;
        alignas(32) float phases[16];alignas(32) uint32_t rgba[16];
        for(int half=0;half<2;++half){
            auto coordinates=_mm256_add_epi32(_mm256_set1_epi32(first+half*8),_mm256_setr_epi32(0,1,2,3,4,5,6,7));
            coordinates=_mm256_min_epi32(_mm256_set1_epi32((horizontal?width:height)-1),_mm256_max_epi32(_mm256_setzero_si256(),coordinates));
            const auto indices=horizontal?_mm256_add_epi32(_mm256_set1_epi32(fixed*width),coordinates):_mm256_add_epi32(_mm256_set1_epi32(fixed),_mm256_mullo_epi32(coordinates,_mm256_set1_epi32(width)));
            const int begin=first+half*8;
            const auto packed=horizontal&&begin>=0&&begin+8<=int(width)
                ?_mm256_loadu_si256(reinterpret_cast<const __m256i*>(pixels+(size_t(fixed)*width+begin)*4))
                :_mm256_i32gather_epi32(reinterpret_cast<const int*>(pixels),indices,4);
            _mm256_store_si256(reinterpret_cast<__m256i*>(rgba+half*8),packed);
            auto phase=_mm256_setzero_ps();
            for(int c=0;c<3;++c){const auto value=_mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srlv_epi32(packed,_mm256_set1_epi32(c*8)),_mm256_set1_epi32(255)));phase=_mm256_add_ps(phase,_mm256_mul_ps(_mm256_sub_ps(value,_mm256_set1_ps(low[c])),_mm256_set1_ps(delta[c]*inverseEnergy)));}
            _mm256_store_ps(phases+half*8,phase);
        }
        float score=1e10f,selected=0;bool found=false;
        for(int k=0;k<10;++k){const int at=first+k;if(at<0||at+1>=int(horizontal?width:height))continue;
            const float a=phases[k],b=phases[k+1];if((a<.5f)==(b<.5f)||(b-a)*normal<=.025f)continue;
            const float t=sat((.5f-a)/(b-a)),position=at+.5f+t;
            const auto* p=reinterpret_cast<const uint8_t*>(rgba+k);const auto* q=p+4;if(p[3]!=255||q[3]!=255)continue;float error=0;
            for(int c=0;c<3;++c){const float d=p[c]+t*(q[c]-p[c])-low[c]-.5f*delta[c];error+=d*d;}
            if(error>192+energy*.12f)continue;
            const float candidate=(position-predicted)*(position-predicted)+error/(64+energy*.1f);
            if(candidate<score){score=candidate;selected=position;found=true;}
        }
        if(found){along[count]=float(line);cross[count]=selected-(horizontal?cx:cy)-.5f;++count;}
    }
    if(count<4)return locateCrossings(pixels,width,height,cx,cy,guide,tolerant);
    // Fit a local quadratic rather than forcing a bend into a short straight
    // segment. Its central position and tangent join neighboring guide cells.
    double s1=0,s2=0,s3=0,s4=0,p0=0,p1=0,p2=0;
    for(int i=0;i<count;++i){const double t=along[i],tt=t*t,p=cross[i];s1+=t;s2+=tt;s3+=tt*t;s4+=tt*tt;p0+=p;p1+=t*p;p2+=tt*p;}
    const double inv=1./count,a=s2-s1*s1*inv,b=s3-s1*s2*inv,c=s4-s2*s2*inv,u=p1-s1*p0*inv,v=p2-s2*p0*inv,det=a*c-b*b;
    if(det<.001)return locateCrossings(pixels,width,height,cx,cy,guide,tolerant);
    const float slope=float((u*c-v*b)/det),curvature=float((v*a-u*b)/det),center=float((p0-slope*s1-curvature*s2)*inv);
    float error=0;for(int i=0;i<count;++i){const float d=cross[i]-center-slope*along[i]-curvature*along[i]*along[i];error+=d*d/count;}
    if(std::abs(slope)>1.5f||std::abs(curvature)>.06f||error>1.5f)return locateCrossings(pixels,width,height,cx,cy,guide,tolerant);
    const float sign=normal<0?-1.f:1.f,scale=sign/std::sqrt(1+slope*slope);
    const float newNx=horizontal?scale:-slope*scale,newNy=horizontal?-slope*scale:scale;
    const float position=(horizontal?cx:cy)+.5f+center;
    guide.v[7]=newNx*confidence;guide.v[8]=newNy*confidence;
    guide.v[9]=(horizontal?-newNx*position-newNy*(cy+.5f):-newNy*position-newNx*(cx+.5f))*confidence;
    guide.v[20]=confidence*sat((1.5f-error)/1.5f)*std::min(1.f,count/4.f);
    guide.v[21]=0;
    return guide;
}
template<bool recoverMissing=true,bool curves=true> inline Guide refined(const uint8_t* pixels,uint32_t width,uint32_t height,int cx,int cy,bool tolerateRinging=false){
    auto broad=analyze(pixels,width,height,cx,cy,4,tolerateRinging);
    bool located=false,fineAnalyzed=false,fineLocated=false;Guide fine;
    // A short dense center probe catches structures between the coarse taps.
    // Keep the broad plane model; refine geometry only when it is supported.
    float range=0;
    const auto* center=pixels+(size_t(std::clamp(cy,0,int(height)-1))*width+std::clamp(cx,0,int(width)-1))*4;
    if(broad.v[0]<.03f){for(auto [dx,dy]:{std::pair{-1,0},std::pair{1,0},std::pair{0,-1},std::pair{0,1}}){const auto* p=pixels+(size_t(std::clamp(cy+dy,0,int(height)-1))*width+std::clamp(cx+dx,0,int(width)-1))*4;for(int c=0;c<3;++c)range=std::max(range,float(std::abs(int(p[c])-center[c])));}}
    if((broad.v[0]>.03f&&broad.v[20]<.85f)||range>12){fine=analyze(pixels,width,height,cx,cy,2,tolerateRinging);fineAnalyzed=true;
        bool supported=true;
        if(tolerateRinging&&broad.v[0]>.03f&&fine.v[0]>.03f){float coarseEnergy=0,fineEnergy=0;for(int c=0;c<3;++c){const float a=(broad.v[4+c]-broad.v[1+c])/broad.v[0],b=(fine.v[4+c]-fine.v[1+c])/fine.v[0];coarseEnergy+=a*a;fineEnergy+=b*b;}supported=fineEnergy>=coarseEnergy*.125f;}
        // A locally neat but much weaker pulse can be compression ringing.
        // It must not replace the color pair of the real surrounding edge.
        if(supported&&fine.v[20]>.10f&&fine.v[20]>broad.v[20]*.8f){
            fine=locateContour<recoverMissing&&curves>(pixels,width,height,cx,cy,oriented(broad,fine),tolerateRinging);fineLocated=true;
            // A neat local fit can fail actual crossing localization. Do not
            // discard the broad model until the fine geometry is validated.
            if(fine.v[20]>.001f){for(int k=10;k<20;++k)fine.v[k]=broad.v[k];fine.v[22]=broad.v[22];broad=fine;located=true;}
        }
    }
    if(!located)broad=locateContour<recoverMissing&&curves>(pixels,width,height,cx,cy,broad,tolerateRinging);
    // Recover omitted local boundaries only where the established selection
    // has no usable geometry. A failed preliminary line must not prevent
    // actual crossing localization of a compatible fine color model.
    if constexpr(recoverMissing)if(tolerateRinging&&broad.v[20]<.03f){
        if(!fineAnalyzed){
            if(broad.v[0]<=.03f)for(auto [dx,dy]:{std::pair{-3,-3},std::pair{3,-3},std::pair{-3,3},std::pair{3,3}}){const auto* p=pixels+(size_t(std::clamp(cy+dy,0,int(height)-1))*width+std::clamp(cx+dx,0,int(width)-1))*4;for(int c=0;c<3;++c)range=std::max(range,float(std::abs(int(p[c])-center[c])));}
            if(broad.v[0]>.03f||range>=8){fine=analyze(pixels,width,height,cx,cy,2,true);fineAnalyzed=true;}
        }
        if(fineAnalyzed&&fine.v[0]>.03f){
            if(!fineLocated)fine=locateContour<recoverMissing&&curves>(pixels,width,height,cx,cy,oriented(broad,fine),true);
            fine.v[20]*=.75f;
            if(fine.v[20]>.03f&&fine.v[20]>broad.v[20]){for(int k=10;k<20;++k)fine.v[k]=broad.v[k];fine.v[22]=broad.v[22];broad=fine;}
        }
    }
    // Geometry must interpolate with its own confidence. A color-compatible
    // but unlocated border model must not pull a valid neighbor's edge.
    const float weight=broad.v[0]>.001f?broad.v[20]/broad.v[0]:0;
    for(int k=7;k<=9;++k)broad.v[k]*=weight;
    return broad;
}
// An extra repair belongs to the colored skirt outside the coverage transition,
// not to every pixel near either endpoint color. Neutral strokes are excluded.
inline __m256 contourSkirtWeight(const __m256 (&low)[3],const __m256 (&delta)[3],
                                __m256 reliability,__m256 ring,float strength){
    const auto zero=_mm256_setzero_ps(),one=_mm256_set1_ps(1);
    if(strength<=0)return zero;
    const auto support=_mm256_mul_ps(reliability,ring);
    if(!_mm256_movemask_ps(_mm256_cmp_ps(support,zero,_CMP_GT_OQ)))return zero;
    const auto spread=_mm256_sub_ps(_mm256_max_ps(delta[0],_mm256_max_ps(delta[1],delta[2])),_mm256_min_ps(delta[0],_mm256_min_ps(delta[1],delta[2])));
    if(!_mm256_movemask_ps(_mm256_cmp_ps(spread,_mm256_set1_ps(80),_CMP_GT_OQ)))return zero;
    auto clamp=[&](__m256 v){return _mm256_min_ps(one,_mm256_max_ps(zero,v));};
    auto neutral=[](__m256 a,__m256 b,__m256 c){const auto lo=_mm256_min_ps(a,_mm256_min_ps(b,c)),hi=_mm256_max_ps(a,_mm256_max_ps(b,c));
        return _mm256_and_ps(_mm256_cmp_ps(lo,_mm256_set1_ps(180),_CMP_GT_OQ),_mm256_cmp_ps(_mm256_sub_ps(hi,lo),_mm256_set1_ps(32),_CMP_LT_OQ));};
    const auto protect=_mm256_or_ps(neutral(low[0],low[1],low[2]),neutral(_mm256_add_ps(low[0],delta[0]),_mm256_add_ps(low[1],delta[1]),_mm256_add_ps(low[2],delta[2])));
    const auto chromatic=clamp(_mm256_mul_ps(_mm256_sub_ps(spread,_mm256_set1_ps(80)),_mm256_set1_ps(1.f/64)));
    return _mm256_andnot_ps(protect,clamp(_mm256_mul_ps(_mm256_set1_ps(1.5f*strength),_mm256_mul_ps(chromatic,support))));
}
inline __m256 contourDetailTrust(const __m256 (&delta)[3],__m256 thin){
    const auto zero=_mm256_setzero_ps(),one=_mm256_set1_ps(1);
    auto clamp=[&](__m256 v){return _mm256_min_ps(one,_mm256_max_ps(zero,v));};
    const auto spread=_mm256_sub_ps(_mm256_max_ps(delta[0],_mm256_max_ps(delta[1],delta[2])),_mm256_min_ps(delta[0],_mm256_min_ps(delta[1],delta[2])));
    const auto color=clamp(_mm256_mul_ps(_mm256_sub_ps(spread,_mm256_set1_ps(32)),_mm256_set1_ps(1.f/48)));
    const auto protectedStroke=clamp(_mm256_sub_ps(one,_mm256_mul_ps(thin,_mm256_set1_ps(4))));
    const auto coverage=clamp(_mm256_sub_ps(one,_mm256_mul_ps(thin,_mm256_set1_ps(.5f))));
    const auto smooth=_mm256_mul_ps(_mm256_mul_ps(coverage,coverage),_mm256_sub_ps(_mm256_set1_ps(3),_mm256_add_ps(coverage,coverage)));
    return _mm256_add_ps(protectedStroke,_mm256_mul_ps(color,_mm256_sub_ps(smooth,protectedStroke)));
}
inline __m256 contourColorCompatibility(const __m256 (&delta)[3],__m256 residual){
    const auto one=_mm256_set1_ps(1),zero=_mm256_setzero_ps();
    const auto spread=_mm256_sub_ps(_mm256_max_ps(delta[0],_mm256_max_ps(delta[1],delta[2])),_mm256_min_ps(delta[0],_mm256_min_ps(delta[1],delta[2])));
    const auto chromatic=_mm256_min_ps(one,_mm256_max_ps(zero,_mm256_mul_ps(_mm256_sub_ps(spread,_mm256_set1_ps(32)),_mm256_set1_ps(1.f/64))));
    return _mm256_min_ps(one,_mm256_max_ps(zero,_mm256_sub_ps(one,_mm256_mul_ps(residual,_mm256_rcp_ps(_mm256_add_ps(_mm256_set1_ps(2700),_mm256_mul_ps(chromatic,_mm256_set1_ps(2700))))))));
}
template<bool legacy=false> inline __m256 contourCoherence(__m256 nx,__m256 ny){
    // Interpolating normals across a genuine 90-degree bend gives length^2
    // about 0.5. Keep a gradual contribution there; opposing normals still
    // collapse to zero and cannot invent a direction through a junction.
    const auto lengthSquared=_mm256_add_ps(_mm256_mul_ps(nx,nx),_mm256_mul_ps(ny,ny));
    if constexpr(legacy)return _mm256_min_ps(_mm256_set1_ps(1),_mm256_max_ps(_mm256_setzero_ps(),_mm256_mul_ps(_mm256_sub_ps(lengthSquared,_mm256_set1_ps(.5f)),_mm256_set1_ps(2))));
    return _mm256_min_ps(_mm256_set1_ps(1),_mm256_max_ps(_mm256_setzero_ps(),lengthSquared));
}
template<int grid, bool cleanSurfaces, bool diagnostic=false, bool legacyCoherence=false> inline void render(uint8_t* pixels,uint32_t width,uint32_t y,const Guide* guides,uint32_t columns,uint32_t rows,float surfaces,float contours) {
    thread_local std::vector<Guide> vertical;vertical.resize(columns+1);
    const float fy=(float(y)-grid/2)/grid;const int row=int(std::floor(fy));const float ty=fy-row;
    const auto* a=guides+size_t(std::clamp(row,0,int(rows)-1))*columns;
    const auto* b=guides+size_t(std::clamp(row+1,0,int(rows)-1))*columns;
    for(uint32_t i=0;i<columns;++i){const auto other=oriented(a[i],b[i]);
        auto interpolate8=[&](int k){_mm256_store_ps(vertical[i].v.data()+k,_mm256_add_ps(_mm256_mul_ps(_mm256_load_ps(a[i].v.data()+k),_mm256_set1_ps(1-ty)),_mm256_mul_ps(_mm256_load_ps(other.v.data()+k),_mm256_set1_ps(ty))));};
        interpolate8(0);
        if constexpr(cleanSurfaces){interpolate8(8);interpolate8(16);}
        else {
            // No surface reconstruction: only normals/intercept (8..9)
            // and geometry/thin confidence (20..21) are needed beyond RGB.
            for(int k:{8,20})_mm_store_ps(vertical[i].v.data()+k,_mm_add_ps(_mm_mul_ps(_mm_load_ps(a[i].v.data()+k),_mm_set1_ps(1-ty)),_mm_mul_ps(_mm_load_ps(other.v.data()+k),_mm_set1_ps(ty))));
        }
    }
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
        float support=l[0]+r[0];if constexpr(cleanSurfaces)support+=surfaces*(l[10]+r[10]);
        if(support<.001f) { if constexpr(diagnostic)for(uint32_t i=0;i<lanes;++i){auto* p=pixels+size_t(x+i)*4;p[0]=p[1]=p[2]=0;p[3]=255;}x+=lanes;continue; }
        const auto fx=_mm256_mul_ps(_mm256_setr_ps(float(x)-cell*grid-grid/2,float(x+1)-cell*grid-grid/2,float(x+2)-cell*grid-grid/2,float(x+3)-cell*grid-grid/2,float(x+4)-cell*grid-grid/2,float(x+5)-cell*grid-grid/2,float(x+6)-cell*grid-grid/2,float(x+7)-cell*grid-grid/2),_mm256_set1_ps(1.f/grid));
        auto field=[&](int k){return _mm256_add_ps(_mm256_set1_ps(l[k]),_mm256_mul_ps(fx,_mm256_set1_ps(r[k]-l[k])));};
        auto* address=pixels+size_t(x)*4;alignas(32) uint8_t tail[32]{};if(lanes<8)std::memcpy(tail,address,lanes*4);
        const auto packed=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lanes==8?address:tail));
        auto channel=[&](int c){return _mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srlv_epi32(packed,_mm256_set1_epi32(c*8)),_mm256_set1_epi32(255)));};
        __m256 result[3]={channel(0),channel(1),channel(2)};
        __m256 diagnosticContour=zero,diagnosticSkirt=zero;
        const auto px=_mm256_setr_ps(float(x)+.5f,float(x)+1.5f,float(x)+2.5f,float(x)+3.5f,float(x)+4.5f,float(x)+5.5f,float(x)+6.5f,float(x)+7.5f),py=_mm256_set1_ps(float(y)+.5f);
        if(l[0]+r[0]>.001f) {
            const auto confidence=field(0),inverse=_mm256_rcp_ps(_mm256_max_ps(_mm256_set1_ps(.001f),confidence));
            __m256 low[3],delta[3];auto energy=one,t=zero;
            for(int c=0;c<3;++c) { low[c]=_mm256_mul_ps(field(1+c),inverse);delta[c]=_mm256_sub_ps(_mm256_mul_ps(field(4+c),inverse),low[c]);energy=_mm256_add_ps(energy,_mm256_mul_ps(delta[c],delta[c]));t=_mm256_add_ps(t,_mm256_mul_ps(_mm256_sub_ps(result[c],low[c]),delta[c])); }
            t=clamp(_mm256_mul_ps(t,_mm256_rcp_ps(energy)));auto residual=zero;
            for(int c=0;c<3;++c) { const auto d=_mm256_sub_ps(result[c],_mm256_add_ps(low[c],_mm256_mul_ps(t,delta[c])));residual=_mm256_add_ps(residual,_mm256_mul_ps(d,d)); }
            const auto compatible=contourColorCompatibility(delta,residual);
            const auto detailGuard=contourDetailTrust(delta,cleanSurfaces||legacyCoherence?field(21):_mm256_mul_ps(field(21),inverse));
            const auto colorReliability=_mm256_mul_ps(detailGuard,_mm256_mul_ps(confidence,compatible));
            auto amount=zero,phase=zero,orientedRing=zero,skirtRing=zero;
            // Rejected geometry still permits color cleanup, but needs no
            // normals, signed distances, reciprocals or phase reconstruction.
            if(l[20]+r[20]>0){
                const auto geometryInverse=cleanSurfaces||legacyCoherence?_mm256_rcp_ps(_mm256_max_ps(_mm256_set1_ps(.001f),field(20))):inverse;
                const auto nx=_mm256_mul_ps(field(7),geometryInverse),ny=_mm256_mul_ps(field(8),geometryInverse);
                const auto distance=_mm256_add_ps(_mm256_mul_ps(field(9),geometryInverse),_mm256_add_ps(_mm256_mul_ps(px,nx),_mm256_mul_ps(py,ny)));
                const auto desired=clamp(_mm256_add_ps(half,_mm256_mul_ps(distance,_mm256_rcp_ps(_mm256_max_ps(_mm256_set1_ps(.85f),_mm256_max_ps(abs(nx),abs(ny)))))));
                const auto coherence=contourCoherence<legacyCoherence>(nx,ny);
                const auto reliability=_mm256_mul_ps(detailGuard,_mm256_mul_ps(field(20),_mm256_mul_ps(coherence,compatible)));
                // Endpoint-valued pixels can be the staircase itself.
                const auto band=clamp(_mm256_sub_ps(_mm256_set1_ps(2.5f),abs(distance)));
                amount=clamp(_mm256_mul_ps(_mm256_set1_ps(contours),_mm256_mul_ps(reliability,band)));
                const auto innerBand=clamp(_mm256_sub_ps(abs(distance),_mm256_set1_ps(1.5f)));
                skirtRing=_mm256_mul_ps(coherence,_mm256_mul_ps(innerBand,clamp(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(14),abs(distance)),_mm256_set1_ps(.125f)))));
                if constexpr(cleanSurfaces)orientedRing=_mm256_mul_ps(coherence,_mm256_mul_ps(innerBand,clamp(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(7),abs(distance)),_mm256_set1_ps(.25f)))));
                phase=_mm256_min_ps(_mm256_set1_ps(.28f),_mm256_max_ps(_mm256_set1_ps(-.28f),_mm256_sub_ps(desired,t)));
            }
            auto phaseCorrection=_mm256_mul_ps(amount,phase);
            auto noiseCorrection=_mm256_mul_ps(_mm256_set1_ps(.18f),amount);
            const auto skirt=contourSkirtWeight(low,delta,colorReliability,skirtRing,std::max(0.f,contours-surfaces));
            if constexpr(diagnostic){diagnosticContour=amount;diagnosticSkirt=skirt;}
            phaseCorrection=_mm256_add_ps(phaseCorrection,_mm256_mul_ps(skirt,_mm256_min_ps(_mm256_set1_ps(.12f),_mm256_max_ps(_mm256_set1_ps(-.12f),phase))));
            noiseCorrection=_mm256_add_ps(noiseCorrection,_mm256_mul_ps(skirt,_mm256_set1_ps(.5f)));
            if constexpr(cleanSurfaces){
                const auto colorBand=clamp(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(.25f),_mm256_min_ps(t,_mm256_sub_ps(one,t))),_mm256_set1_ps(4)));
                const auto cleanup=clamp(_mm256_mul_ps(_mm256_set1_ps(.8f*surfaces),_mm256_mul_ps(colorReliability,_mm256_max_ps(colorBand,orientedRing))));
                const auto endpoint=_mm256_and_ps(_mm256_cmp_ps(t,half,_CMP_GE_OQ),one);
                const auto ringing=_mm256_min_ps(_mm256_set1_ps(.15f),_mm256_max_ps(_mm256_set1_ps(-.15f),_mm256_sub_ps(endpoint,t)));
                phaseCorrection=_mm256_add_ps(phaseCorrection,_mm256_mul_ps(cleanup,ringing));
                noiseCorrection=_mm256_add_ps(noiseCorrection,_mm256_mul_ps(_mm256_set1_ps(.35f),cleanup));
            }
            for(int c=0;c<3;++c) { const auto own=result[c],projected=_mm256_add_ps(low[c],_mm256_mul_ps(t,delta[c]));const auto correction=_mm256_add_ps(_mm256_mul_ps(phaseCorrection,delta[c]),_mm256_mul_ps(noiseCorrection,_mm256_sub_ps(projected,own)));const auto lo=_mm256_min_ps(own,_mm256_min_ps(low[c],_mm256_add_ps(low[c],delta[c]))),hi=_mm256_max_ps(own,_mm256_max_ps(low[c],_mm256_add_ps(low[c],delta[c])));result[c]=_mm256_min_ps(hi,_mm256_max_ps(lo,_mm256_add_ps(own,correction))); }
        }
        if(cleanSurfaces&&surfaces&&l[10]+r[10]>.001f) {
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
        if constexpr(diagnostic){result[0]=_mm256_mul_ps(diagnosticSkirt,_mm256_set1_ps(255));result[1]=_mm256_mul_ps(diagnosticContour,_mm256_set1_ps(255));result[2]=zero;}
        __m256i values[3];for(int c=0;c<3;++c)values[c]=_mm256_cvttps_epi32(_mm256_add_ps(_mm256_min_ps(_mm256_set1_ps(255),_mm256_max_ps(zero,result[c])),half));
        const auto output=_mm256_or_si256(_mm256_and_si256(packed,_mm256_set1_epi32(int(0xff000000u))),_mm256_or_si256(values[0],_mm256_or_si256(_mm256_slli_epi32(values[1],8),_mm256_slli_epi32(values[2],16))));
        const auto opaque=_mm256_cmpeq_epi32(_mm256_srli_epi32(packed,24),_mm256_set1_epi32(255));
        __m256i final;
        if constexpr(diagnostic)final=_mm256_or_si256(_mm256_set1_epi32(int(0xff000000u)),_mm256_and_si256(output,opaque));
        else final=_mm256_blendv_epi8(packed,output,opaque);
        if(lanes==8)_mm256_storeu_si256(reinterpret_cast<__m256i*>(address),final);else { _mm256_store_si256(reinterpret_cast<__m256i*>(tail),final);std::memcpy(address,tail,lanes*4); }
        x+=lanes;
    }
    _mm256_zeroupper();
}
template<int grid> inline void regularizeGuides(std::vector<Guide>& guides,uint32_t columns,uint32_t rows,uint32_t workers){
    thread_local std::vector<Guide> original;original=guides;const auto& source=original;
    // Every accepted guide is visited by up to 24 neighbors. Normalize its
    // colors and line once, retaining the frozen weighted values for the
    // orientation test and the same divisions/rounding as the original fit.
    struct Normalized {float nx,ny,intercept,a[3],b[3];};
    thread_local std::vector<Normalized> normalized;normalized.resize(source.size());
    auto* cached=normalized.data();
    parallelFor(rows,workers,[&](uint32_t gy){for(uint32_t gx=0;gx<columns;++gx){
        const size_t at=size_t(gy)*columns+gx;const auto& g=source[at];
        if(g.v[20]<.12f||g.v[21]>.1f)continue;
        auto& n=cached[at];n.nx=g.v[7]/g.v[20];n.ny=g.v[8]/g.v[20];n.intercept=g.v[9]/g.v[20];
        for(int c=0;c<3;++c){n.a[c]=g.v[1+c]/g.v[0];n.b[c]=g.v[4+c]/g.v[0];}
    }});
    parallelFor(rows,workers,[&](uint32_t gy){for(uint32_t gx=0;gx<columns;++gx){
        const auto& own=source[size_t(gy)*columns+gx];const float confidence=own.v[20];
        if(confidence<.12f||own.v[21]>.1f)continue;
        const auto& ownFit=cached[size_t(gy)*columns+gx];
        const float nx=ownFit.nx,ny=ownFit.ny;
        const float cx=float(gx*grid+grid/2)+.5f,cy=float(gy*grid+grid/2)+.5f;
        const float distance=nx*cx+ny*cy+ownFit.intercept;
        if(std::abs(distance)>grid)continue;
        const float px=cx-nx*distance,py=cy-ny*distance,tx=-ny,ty=nx;
        const auto* aColor=ownFit.a;const auto* bColor=ownFit.b;
        double matrix[3][4]{};matrix[0][0]=1;matrix[1][1]=.1;matrix[2][2]=.25;
        bool negative=false,positive=false;int count=0;
        for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx){
            if((!dx&&!dy)||int(gx)+dx<0||int(gy)+dy<0||int(gx)+dx>=int(columns)||int(gy)+dy>=int(rows))continue;
            const auto& other=source[size_t(int(gy)+dy)*columns+int(gx)+dx];const float weight=other.v[20];if(weight<.12f||other.v[21]>.1f)continue;
            const auto& otherFit=cached[size_t(int(gy)+dy)*columns+int(gx)+dx];
            float alignment=0;for(int c=0;c<3;++c)alignment+=(bColor[c]-aColor[c])*(other.v[4+c]-other.v[1+c]);
            const bool reverse=alignment<0;const float sign=reverse?-1.f:1.f;
            const float ox=sign*otherFit.nx,oy=sign*otherFit.ny,agreement=ox*nx+oy*ny;if(agreement<.75f)continue;
            float colorError=0;for(int c=0;c<3;++c){colorError=std::max(colorError,std::abs((reverse?otherFit.b:otherFit.a)[c]-aColor[c]));colorError=std::max(colorError,std::abs((reverse?otherFit.a:otherFit.b)[c]-bColor[c]));}if(colorError>=40)continue;
            const float qx=cx+dx*grid,qy=cy+dy*grid,offset=ox*qx+oy*qy+sign*otherFit.intercept;
            const float ex=qx-ox*offset-px,ey=qy-oy*offset-py;
            const float t=(ex*tx+ey*ty)/grid,d=ex*nx+ey*ny;
            if(std::abs(d)>4||std::abs(t)>2.5f||std::abs(t)<.4f)continue;
            const double w=weight*agreement*(1-colorError/40)/(1+t*t*.15f),v[3]{1,t,t*t};
            for(int r=0;r<3;++r){for(int c=0;c<3;++c)matrix[r][c]+=w*v[r]*v[c];matrix[r][3]+=w*v[r]*d;}
            negative|=t<-.5f;positive|=t>.5f;++count;
        }
        if(count<4||!negative||!positive)continue;
        bool valid=true;
        for(int p=0;p<3;++p){if(std::abs(matrix[p][p])<.001){valid=false;break;}const double divisor=matrix[p][p];for(int c=p;c<4;++c)matrix[p][c]/=divisor;for(int r=0;r<3;++r)if(r!=p){const double m=matrix[r][p];for(int c=p;c<4;++c)matrix[r][c]-=m*matrix[p][c];}}
        if(!valid)continue;
        const float move=std::clamp(float(matrix[0][3]),-1.5f,1.5f)*.85f;
        const float slope=std::clamp(float(matrix[1][3])/grid,-.35f,.35f)*.85f;
        const float inverse=1/std::sqrt(1+slope*slope),newNx=(nx-slope*tx)*inverse,newNy=(ny-slope*ty)*inverse;
        auto& out=guides[size_t(gy)*columns+gx];out.v[7]=newNx*confidence;out.v[8]=newNy*confidence;
        out.v[9]=(-newNx*(px+move*nx)-newNy*(py+move*ny))*confidence;
    }});
}
template<int grid,bool recovery=true> inline void buildGuides(const uint8_t* pixels,uint32_t width,uint32_t height,uint32_t workers,float surfaces,float contours,bool tolerateRinging,std::vector<Guide>& guides){
    const auto columns=(width+grid-1)/grid,rows=(height+grid-1)/grid;guides.resize(size_t(columns)*rows);
    parallelFor(rows,workers,[&](uint32_t y){for(uint32_t x=0;x<columns;++x){auto g=contours>.8f?refined<recovery,true>(pixels,width,height,int(x*grid+grid/2),int(y*grid+grid/2),tolerateRinging):refined<recovery,false>(pixels,width,height,int(x*grid+grid/2),int(y*grid+grid/2),tolerateRinging);guides[size_t(y)*columns+x]=surfaces||!recovery?g:contourOnly(g);}});
    if constexpr(recovery)if(!surfaces&&tolerateRinging&&contours>.8f)regularizeGuides<grid>(guides,columns,rows,workers);
}
template<int grid> inline void restoreGrid(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,float surfaces,float contours,bool tolerateRinging=false) {
    if(!surfaces&&!contours)return;
    TimingTimeline timing;const auto columns=(width+grid-1)/grid,rows=(height+grid-1)/grid;
    thread_local std::vector<Guide> cache;auto& guides=cache;
    buildGuides<grid>(pixels.data(),width,height,workers,surfaces,contours,tolerateRinging,guides);
    timing.mark("decode.unified385.guide");
    parallelFor((height+15)/16,workers,[&](uint32_t job){for(uint32_t y=job*16;y<std::min(height,(job+1)*16);++y){if(surfaces)render<grid,true>(pixels.data()+size_t(y)*width*4,width,y,guides.data(),columns,rows,surfaces,contours);
        else render<grid,false>(pixels.data()+size_t(y)*width*4,width,y,guides.data(),columns,rows,0,contours);}});timing.mark("decode.unified385.render");
}
inline void restore(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,float surfaces,float contours,bool tolerateRinging=false){
    // Weak corrections need fewer overlapping guides, not another algorithm.
    if(contourGuideSpacing(surfaces,contours)==16)restoreGrid<16>(pixels,width,height,workers,surfaces,contours,tolerateRinging);
    else restoreGrid<8>(pixels,width,height,workers,surfaces,contours,tolerateRinging);
}
}
