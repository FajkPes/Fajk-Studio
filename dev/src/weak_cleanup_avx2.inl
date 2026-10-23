// Weak medium-rate corrections need only the immediate neighborhood. Read an
// immutable source and use one RGB weight per tap, preserving chromatic edges.
static void weakCleanup(std::span<uint8_t> pixels,std::span<const uint8_t> source,
                        uint32_t width,uint32_t height,uint32_t workers,float amount){
    parallelFor((height+15)/16,workers,[&](uint32_t job){
        const auto zero=_mm256_setzero_ps(),one=_mm256_set1_ps(1);
        const auto mask=_mm256_set1_epi32(255);
        auto channel=[&](__m256i p,int c){return _mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srlv_epi32(p,_mm256_set1_epi32(c*8)),mask));};
        for(uint32_t y=job*16;y<std::min(height,job*16+16);++y)for(uint32_t x=0;x<width;x+=8){
            auto load=[&](int dx,int dy){const auto* row=reinterpret_cast<const int*>(source.data()+size_t(std::clamp(int(y)+dy,0,int(height)-1))*width*4);
                if(int(x)+dx>=0&&int64_t(x)+dx+8<=width)return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row+int(x)+dx));
                auto indices=_mm256_min_epi32(_mm256_set1_epi32(width-1),_mm256_max_epi32(_mm256_setzero_si256(),_mm256_add_epi32(_mm256_set1_epi32(int(x)+dx),_mm256_setr_epi32(0,1,2,3,4,5,6,7))));
                return _mm256_i32gather_epi32(row,indices,4);};
            const auto own=load(0,0);__m256 original[3],sum[3]{zero,zero,zero};auto weight=one;
            for(int c=0;c<3;++c)original[c]=channel(own,c);
            for(auto [dx,dy]:{std::pair{-1,0},std::pair{1,0},std::pair{0,-1},std::pair{0,1}}){
                const auto tap=load(dx,dy),d=_mm256_or_si256(_mm256_subs_epu8(tap,own),_mm256_subs_epu8(own,tap));
                const auto maxd=_mm256_and_si256(mask,_mm256_max_epu8(d,_mm256_max_epu8(_mm256_srli_epi32(d,8),_mm256_srli_epi32(d,16))));
                auto t=_mm256_mul_ps(_mm256_cvtepi32_ps(maxd),_mm256_set1_ps(1.f/32));
                t=_mm256_max_ps(zero,_mm256_sub_ps(one,_mm256_mul_ps(t,t)));t=_mm256_mul_ps(t,t);
                t=_mm256_and_ps(t,_mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_srli_epi32(_mm256_and_si256(own,tap),24),mask)));
                weight=_mm256_add_ps(weight,t);
                for(int c=0;c<3;++c)sum[c]=_mm256_add_ps(sum[c],_mm256_mul_ps(t,_mm256_sub_ps(channel(tap,c),original[c])));
            }
            const auto scale=_mm256_div_ps(_mm256_set1_ps(amount),weight);auto out=_mm256_and_si256(own,_mm256_set1_epi32(int(0xff000000u)));
            for(int c=0;c<3;++c){auto value=_mm256_add_ps(original[c],_mm256_mul_ps(scale,sum[c]));auto rounded=_mm256_cvttps_epi32(_mm256_add_ps(value,_mm256_set1_ps(.5f)));out=_mm256_or_si256(out,_mm256_sllv_epi32(rounded,_mm256_set1_epi32(c*8)));}
            const auto at=(size_t(y)*width+x)*4;
            if(x+8<=width)_mm256_storeu_si256(reinterpret_cast<__m256i*>(pixels.data()+at),out);
            else {alignas(32)uint8_t tail[32];_mm256_store_si256(reinterpret_cast<__m256i*>(tail),out);std::memcpy(pixels.data()+at,tail,(width-x)*4);}
        }
    });_mm256_zeroupper();
}
