// Included only by the AVX2 translation unit. Eight independent pixels keep
// the scalar 4342 operation order; no FMA, reciprocal approximation or AA change.
namespace fajk {
uint32_t colorBoundaryRowAvx2(const uint8_t* input,uint8_t* output,uint32_t width,uint32_t y,uint32_t x){
    struct Color { __m256 c[3]; };
    const auto zero=_mm256_setzero_ps(),one=_mm256_set1_ps(1.f),half=_mm256_set1_ps(.5f);
    const auto bytes=_mm256_set1_epi32(255);
    auto unpack=[&](__m256i packed){Color value;value.c[0]=_mm256_cvtepi32_ps(_mm256_and_si256(packed,bytes));value.c[1]=_mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(packed,8),bytes));value.c[2]=_mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(packed,16),bytes));return value;};
    auto square=[&](const Color& a,const Color& b){auto sum=zero;for(int c=0;c<3;++c){const auto delta=_mm256_sub_ps(a.c[c],b.c[c]);sum=_mm256_add_ps(sum,_mm256_mul_ps(delta,delta));}return sum;};
    auto clamp=[&](__m256 value){return _mm256_min_ps(one,_mm256_max_ps(zero,value));};
    for(;x+8<=width-3;x+=8){
        const auto* base=reinterpret_cast<const int*>(input+(size_t(y)*width+x)*4);
        const auto packed=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(base));const auto center=unpack(packed);
        auto load=[&](int offset){return unpack(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(base+offset)));};
        auto flat=_mm256_castsi256_ps(_mm256_set1_epi32(-1));
        for(int offset:{-1,1,-int(width),int(width)})flat=_mm256_and_ps(flat,_mm256_cmp_ps(square(center,load(offset)),_mm256_set1_ps(9.f),_CMP_LT_OQ));
        if(_mm256_movemask_ps(flat)==255)continue;
        Color low{},high{};auto best=zero;auto direction=_mm256_set1_epi32(1);
        for(int d:{1,int(width),int(width)+1,1-int(width)}){
            const auto a=load(-2*d),b=load(2*d);auto gradient=square(a,b);
            if(d!=1&&d!=int(width))gradient=_mm256_div_ps(gradient,_mm256_set1_ps(2.f));
            const auto select=_mm256_cmp_ps(gradient,best,_CMP_GT_OQ);best=_mm256_max_ps(best,gradient);
            direction=_mm256_blendv_epi8(direction,_mm256_set1_epi32(d),_mm256_castps_si256(select));
            for(int c=0;c<3;++c){low.c[c]=_mm256_blendv_ps(low.c[c],a.c[c],select);high.c[c]=_mm256_blendv_ps(high.c[c],b.c[c],select);}
        }
        auto energy=square(low,high);auto active=_mm256_andnot_ps(flat,_mm256_cmp_ps(energy,_mm256_set1_ps(900.f),_CMP_GT_OQ));
        if(!_mm256_movemask_ps(active))continue;
        energy=_mm256_max_ps(one,energy);
        auto along=[&](int distance){const auto offsets=_mm256_add_epi32(_mm256_setr_epi32(0,1,2,3,4,5,6,7),_mm256_mullo_epi32(direction,_mm256_set1_epi32(distance)));return unpack(_mm256_i32gather_epi32(base,offsets,4));};
        const auto sideError=_mm256_max_ps(square(low,along(-3)),square(high,along(3)));
        const auto flatConfidence=clamp(_mm256_sub_ps(one,_mm256_div_ps(sideError,_mm256_add_ps(_mm256_mul_ps(energy,_mm256_set1_ps(.06f)),one))));
        active=_mm256_and_ps(active,_mm256_cmp_ps(flatConfidence,zero,_CMP_GT_OQ));
        if(!_mm256_movemask_ps(active))continue;
        Color difference;for(int c=0;c<3;++c)difference.c[c]=_mm256_sub_ps(high.c[c],low.c[c]);
        auto projection=[&](const Color& p){auto dot=zero;for(int c=0;c<3;++c)dot=_mm256_add_ps(dot,_mm256_mul_ps(_mm256_sub_ps(p.c[c],low.c[c]),difference.c[c]));return _mm256_div_ps(dot,energy);};
        const auto t=projection(center);active=_mm256_and_ps(active,_mm256_and_ps(_mm256_cmp_ps(t,_mm256_set1_ps(.02f),_CMP_GT_OQ),_mm256_cmp_ps(t,_mm256_set1_ps(.98f),_CMP_LT_OQ)));
        if(!_mm256_movemask_ps(active))continue;
        auto residual=zero;
        for(const auto& sample:{along(-1),center,along(1)}){const auto at=clamp(projection(sample));Color predicted;for(int c=0;c<3;++c)predicted.c[c]=_mm256_add_ps(low.c[c],_mm256_mul_ps(at,difference.c[c]));residual=_mm256_max_ps(residual,square(predicted,sample));}
        const auto lineConfidence=clamp(_mm256_sub_ps(one,_mm256_div_ps(residual,_mm256_add_ps(_mm256_mul_ps(energy,_mm256_set1_ps(.02f)),_mm256_set1_ps(4.f)))));
        const auto contrast=clamp(_mm256_div_ps(_mm256_sub_ps(_mm256_sqrt_ps(energy),_mm256_set1_ps(30.f)),_mm256_set1_ps(60.f)));
        const auto target=clamp(_mm256_add_ps(_mm256_mul_ps(_mm256_sub_ps(t,half),_mm256_set1_ps(3.f)),half));
        auto shift=_mm256_mul_ps(_mm256_mul_ps(_mm256_mul_ps(_mm256_sub_ps(target,t),flatConfidence),lineConfidence),contrast);
        shift=_mm256_and_ps(shift,active);__m256i channels[3];
        for(int c=0;c<3;++c){const auto lo=_mm256_min_ps(center.c[c],_mm256_min_ps(low.c[c],high.c[c])),hi=_mm256_max_ps(center.c[c],_mm256_max_ps(low.c[c],high.c[c]));const auto value=_mm256_min_ps(hi,_mm256_max_ps(lo,_mm256_add_ps(center.c[c],_mm256_mul_ps(shift,difference.c[c]))));channels[c]=_mm256_cvttps_epi32(_mm256_add_ps(value,half));}
        const auto rgb=_mm256_or_si256(channels[0],_mm256_or_si256(_mm256_slli_epi32(channels[1],8),_mm256_slli_epi32(channels[2],16)));
        const auto rgba=_mm256_or_si256(rgb,_mm256_and_si256(packed,_mm256_set1_epi32(int(0xff000000u))));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(output+(size_t(y)*width+x)*4),rgba);
    }return x;
}
}
