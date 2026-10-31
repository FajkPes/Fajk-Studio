// Follow the dominant RGB edge tangent in the existing tile-output pass.
// Two symmetric same-side samples suppress stair steps; a shared RGB range gate stops
// averaging across a different object. Immutable input makes worker order irrelevant.
// The weighted update is a convex combination, so it cannot create overshoot.
static void contourTangent8(float* const* planes,uint32_t pitch,uint32_t w,uint32_t h,
                              uint32_t x,uint32_t y,float strength,__m256 (&clean)[3]){
    const auto zero=_mm256_setzero_ps(),one=_mm256_set1_ps(1);
    auto at=[&](int c,int dx,int dy){
        const int yy=std::clamp(int(y)+dy,0,int(h)-1),xx=int(x)+dx;
        if(xx>=0&&xx+8<=int(w))return _mm256_loadu_ps(planes[c]+size_t(yy)*pitch+xx);
        const auto indices=_mm256_min_epi32(_mm256_set1_epi32(w-1),_mm256_max_epi32(_mm256_setzero_si256(),
            _mm256_add_epi32(_mm256_set1_epi32(xx),_mm256_setr_epi32(0,1,2,3,4,5,6,7))));
        return _mm256_i32gather_ps(planes[c]+size_t(yy)*pitch,indices,4);
    };
    for(int c=0;c<3;++c)clean[c]=at(c,0,0);
    if(!strength)return;
    __m256 gx=zero,gy=zero,best=zero;
    for(int c=0;c<3;++c){
        auto dx=_mm256_sub_ps(at(c,1,0),at(c,-1,0)),dy=_mm256_sub_ps(at(c,0,1),at(c,0,-1));
        auto e=_mm256_add_ps(_mm256_mul_ps(dx,dx),_mm256_mul_ps(dy,dy));
        auto select=_mm256_cmp_ps(e,best,_CMP_GT_OQ);
        gx=_mm256_blendv_ps(gx,dx,select);gy=_mm256_blendv_ps(gy,dy,select);best=_mm256_max_ps(best,e);
    }
    if(!_mm256_movemask_ps(_mm256_cmp_ps(best,_mm256_set1_ps(64),_CMP_GT_OQ)))return;
    const auto squared=_mm256_add_ps(best,one);
    auto inverse=_mm256_rsqrt_ps(squared);
    inverse=_mm256_mul_ps(inverse,_mm256_sub_ps(_mm256_set1_ps(1.5f),
        _mm256_mul_ps(_mm256_mul_ps(_mm256_set1_ps(.5f),squared),_mm256_mul_ps(inverse,inverse))));
    const auto tx=_mm256_mul_ps(_mm256_sub_ps(zero,gy),inverse),ty=_mm256_mul_ps(gx,inverse);
    const auto px=_mm256_cvtepi32_ps(_mm256_add_epi32(_mm256_set1_epi32(x),_mm256_setr_epi32(0,1,2,3,4,5,6,7)));
    __m256 sum[3]{zero,zero,zero},total=zero;
    for(float distance:{-2.f,2.f}){
        auto xx=_mm256_cvttps_epi32(_mm256_floor_ps(_mm256_add_ps(_mm256_add_ps(px,_mm256_mul_ps(tx,_mm256_set1_ps(distance))),_mm256_set1_ps(.5f))));
        auto yy=_mm256_cvttps_epi32(_mm256_floor_ps(_mm256_add_ps(_mm256_add_ps(_mm256_set1_ps(float(y)),_mm256_mul_ps(ty,_mm256_set1_ps(distance))),_mm256_set1_ps(.5f))));
        xx=_mm256_min_epi32(_mm256_set1_epi32(w-1),_mm256_max_epi32(_mm256_setzero_si256(),xx));
        yy=_mm256_min_epi32(_mm256_set1_epi32(h-1),_mm256_max_epi32(_mm256_setzero_si256(),yy));
        auto indices=_mm256_add_epi32(xx,_mm256_mullo_epi32(yy,_mm256_set1_epi32(pitch)));
        __m256 delta[3],difference=zero;
        for(int c=0;c<3;++c){delta[c]=_mm256_sub_ps(_mm256_i32gather_ps(planes[c],indices,4),clean[c]);difference=_mm256_max_ps(difference,_mm256_andnot_ps(_mm256_set1_ps(-0.f),delta[c]));}
        auto weight=_mm256_max_ps(zero,_mm256_sub_ps(one,_mm256_mul_ps(difference,_mm256_set1_ps(1.f/48))));
        weight=_mm256_mul_ps(weight,weight);
        total=_mm256_add_ps(total,weight);
        for(int c=0;c<3;++c)sum[c]=_mm256_add_ps(sum[c],_mm256_mul_ps(weight,delta[c]));
    }
    auto active=_mm256_min_ps(one,_mm256_max_ps(zero,_mm256_mul_ps(_mm256_sub_ps(best,_mm256_set1_ps(64)),_mm256_set1_ps(1.f/512))));
    auto scale=_mm256_div_ps(_mm256_mul_ps(active,_mm256_set1_ps(std::min(1.f,strength*2))),_mm256_max_ps(one,total));
    for(int c=0;c<3;++c)clean[c]=_mm256_add_ps(clean[c],_mm256_mul_ps(scale,sum[c]));
}
