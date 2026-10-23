namespace fajk {
uint32_t guidedChromaRowAvx2(const float* luma,const float* g0,const float* g1,const float* u0,const float* u1,const float* v0,const float* v1,int last,uint32_t width,float inverseGuide,float* outU,float* outV,uint8_t model){
    const auto zero=_mm256_setzero_ps(),one=_mm256_set1_ps(1),inv=_mm256_set1_ps(inverseGuide);
    auto reciprocal=[&](__m256 value){const auto estimate=_mm256_rcp_ps(value);
        return _mm256_mul_ps(estimate,_mm256_sub_ps(_mm256_set1_ps(2),_mm256_mul_ps(value,estimate)));};
    const float basis[4]={.5625f,.1875f,.1875f,.0625f};uint32_t x=0;
    for(;x+8<=width;x+=8){const int cx=int(x/2);const auto light=_mm256_loadu_ps(luma+x);
        auto expand=[&](const float* row,__m256& center,__m256& neighbor){
            if(cx>=1&&cx+6<=last){const auto block=_mm256_loadu_ps(row+cx-1);center=_mm256_permutevar8x32_ps(block,_mm256_setr_epi32(1,1,2,2,3,3,4,4));neighbor=_mm256_permutevar8x32_ps(block,_mm256_setr_epi32(0,2,1,3,2,4,3,5));}
            else {center=_mm256_i32gather_ps(row,_mm256_setr_epi32(cx,cx,cx+1,cx+1,cx+2,cx+2,cx+3,cx+3),4);neighbor=_mm256_i32gather_ps(row,_mm256_setr_epi32(std::max(0,cx-1),cx+1,cx,cx+2,cx+1,cx+3,cx+2,std::min(last,cx+4)),4);}
        };
        __m256 g[4];expand(g0,g[0],g[1]);expand(g1,g[2],g[3]);__m256 weights[4];auto total=zero;
        for(int i=0;i<4;++i){const auto d=_mm256_sub_ps(light,g[i]);weights[i]=_mm256_mul_ps(_mm256_set1_ps(basis[i]),reciprocal(_mm256_add_ps(one,_mm256_mul_ps(_mm256_mul_ps(d,d),inv))));total=_mm256_add_ps(total,weights[i]);}
        const auto inverseTotal=reciprocal(total);
        auto mean=zero,variance=zero;if(model){for(int i=0;i<4;++i)mean=_mm256_add_ps(mean,_mm256_mul_ps(weights[i],g[i]));mean=_mm256_div_ps(mean,total);for(int i=0;i<4;++i){const auto d=_mm256_sub_ps(g[i],mean);variance=_mm256_add_ps(variance,_mm256_mul_ps(_mm256_mul_ps(weights[i],d),d));}variance=_mm256_div_ps(variance,total);}
        auto channel=[&](const float* a,const float* b,float* out){__m256 values[4];expand(a,values[0],values[1]);expand(b,values[2],values[3]);auto sum=zero;for(int i=0;i<4;++i)sum=_mm256_add_ps(sum,_mm256_mul_ps(weights[i],values[i]));auto average=_mm256_mul_ps(sum,inverseTotal);if(model){auto covariance=zero;for(int i=0;i<4;++i)covariance=_mm256_add_ps(covariance,_mm256_mul_ps(_mm256_mul_ps(weights[i],_mm256_sub_ps(g[i],mean)),_mm256_sub_ps(values[i],average)));covariance=_mm256_div_ps(covariance,total);const auto low=_mm256_min_ps(_mm256_min_ps(values[0],values[1]),_mm256_min_ps(values[2],values[3])),high=_mm256_max_ps(_mm256_max_ps(values[0],values[1]),_mm256_max_ps(values[2],values[3]));const auto correction=_mm256_mul_ps(_mm256_div_ps(_mm256_mul_ps(_mm256_set1_ps(model==1?.5f:model==2?1.f:1.5f),covariance),_mm256_add_ps(variance,_mm256_set1_ps(16.f))),_mm256_sub_ps(light,mean));average=_mm256_min_ps(high,_mm256_max_ps(low,_mm256_add_ps(average,correction)));}_mm256_storeu_ps(out+x,average);};
        channel(u0,u1,outU);channel(v0,v1,outV);
    }return x;
}
}
