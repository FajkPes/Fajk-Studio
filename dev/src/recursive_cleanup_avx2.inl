// Eight independent recursive rows share a contiguous, transposed cache block.
// Within each row, the forward/backward operations match the SSE reference.
namespace fajk {
static void transposeCleanup8(__m256 (&v)[8]){
    const auto a0=_mm256_unpacklo_ps(v[0],v[1]),a1=_mm256_unpackhi_ps(v[0],v[1]);
    const auto a2=_mm256_unpacklo_ps(v[2],v[3]),a3=_mm256_unpackhi_ps(v[2],v[3]);
    const auto a4=_mm256_unpacklo_ps(v[4],v[5]),a5=_mm256_unpackhi_ps(v[4],v[5]);
    const auto a6=_mm256_unpacklo_ps(v[6],v[7]),a7=_mm256_unpackhi_ps(v[6],v[7]);
    const auto b0=_mm256_shuffle_ps(a0,a2,0x44),b1=_mm256_shuffle_ps(a0,a2,0xee);
    const auto b2=_mm256_shuffle_ps(a1,a3,0x44),b3=_mm256_shuffle_ps(a1,a3,0xee);
    const auto b4=_mm256_shuffle_ps(a4,a6,0x44),b5=_mm256_shuffle_ps(a4,a6,0xee);
    const auto b6=_mm256_shuffle_ps(a5,a7,0x44),b7=_mm256_shuffle_ps(a5,a7,0xee);
    v[0]=_mm256_permute2f128_ps(b0,b4,0x20);v[1]=_mm256_permute2f128_ps(b1,b5,0x20);
    v[2]=_mm256_permute2f128_ps(b2,b6,0x20);v[3]=_mm256_permute2f128_ps(b3,b7,0x20);
    v[4]=_mm256_permute2f128_ps(b0,b4,0x31);v[5]=_mm256_permute2f128_ps(b1,b5,0x31);
    v[6]=_mm256_permute2f128_ps(b2,b6,0x31);v[7]=_mm256_permute2f128_ps(b3,b7,0x31);
}
void recursiveLumaHorizontalAvx2(float* data,const float* original,int stride,int width,int first,float range,float alpha){
    thread_local std::array<std::vector<float>,2> cache;
    auto& source=cache[0];auto& filtered=cache[1];source.resize(size_t(width)*8);filtered.resize(source.size());
    int x=0;
    for(;x+8<=width;x+=8){__m256 rows[8];for(int r=0;r<8;++r)rows[r]=_mm256_loadu_ps(original+size_t(first+r)*stride+x);transposeCleanup8(rows);for(int c=0;c<8;++c)_mm256_storeu_ps(source.data()+size_t(x+c)*8,rows[c]);}
    for(;x<width;++x)for(int r=0;r<8;++r)source[size_t(x)*8+r]=original[size_t(first+r)*stride+x];
    const auto sign=_mm256_set1_ps(-0.f),one=_mm256_set1_ps(1),squared=_mm256_set1_ps(range*range),gain=_mm256_set1_ps(alpha),limit=_mm256_set1_ps(4*range);
    auto weight=[&](__m256 a,__m256 b){const auto delta=_mm256_andnot_ps(sign,_mm256_sub_ps(a,b)),gate=_mm256_add_ps(one,_mm256_div_ps(_mm256_mul_ps(delta,delta),squared));return _mm256_and_ps(_mm256_cmp_ps(delta,limit,_CMP_LT_OQ),_mm256_div_ps(gain,_mm256_mul_ps(gate,gate)));};
    auto previousSource=_mm256_loadu_ps(source.data()),previous=previousSource;_mm256_storeu_ps(filtered.data(),previous);
    for(x=1;x<width;++x){const auto center=_mm256_loadu_ps(source.data()+size_t(x)*8);previous=_mm256_add_ps(center,_mm256_mul_ps(weight(center,previousSource),_mm256_sub_ps(previous,center)));_mm256_storeu_ps(filtered.data()+size_t(x)*8,previous);previousSource=center;}
    for(x=width-2;x>=0;--x){const auto centerSource=_mm256_loadu_ps(source.data()+size_t(x)*8),center=_mm256_loadu_ps(filtered.data()+size_t(x)*8);previous=_mm256_add_ps(center,_mm256_mul_ps(weight(centerSource,previousSource),_mm256_sub_ps(previous,center)));_mm256_storeu_ps(filtered.data()+size_t(x)*8,previous);previousSource=centerSource;}
    x=0;for(;x+8<=width;x+=8){__m256 cols[8];for(int c=0;c<8;++c)cols[c]=_mm256_loadu_ps(filtered.data()+size_t(x+c)*8);transposeCleanup8(cols);for(int r=0;r<8;++r)_mm256_storeu_ps(data+size_t(first+r)*stride+x,cols[r]);}
    for(;x<width;++x)for(int r=0;r<8;++r)data[size_t(first+r)*stride+x]=filtered[size_t(x)*8+r];
}
template<int Channels> static void recursiveVerticalCleanupAvx2(float* const* data,const float* const* original,const float* guide,int stride,int width,int height,int begin,int end,float range,float alpha){
    const auto sign=_mm256_set1_ps(-0.f),one=_mm256_set1_ps(1),half=_mm256_set1_ps(.5f),squared=_mm256_set1_ps(range*range),gain=_mm256_set1_ps(alpha),limit=_mm256_set1_ps(4*range);
    auto update=[&](int y,int adjacent,int x){const size_t at=size_t(y)*stride+x,neighbor=size_t(adjacent)*stride+x,src=size_t(y)*width+x,other=size_t(adjacent)*width+x;
        auto delta=_mm256_andnot_ps(sign,_mm256_sub_ps(_mm256_loadu_ps(original[0]+src),_mm256_loadu_ps(original[0]+other)));
        if constexpr(Channels==2){delta=_mm256_max_ps(delta,_mm256_andnot_ps(sign,_mm256_sub_ps(_mm256_loadu_ps(original[1]+src),_mm256_loadu_ps(original[1]+other))));const auto dg=guide?_mm256_mul_ps(half,_mm256_andnot_ps(sign,_mm256_sub_ps(_mm256_loadu_ps(guide+at),_mm256_loadu_ps(guide+neighbor)))):_mm256_setzero_ps();delta=_mm256_max_ps(delta,dg);}
        const auto gate=_mm256_add_ps(one,_mm256_div_ps(_mm256_mul_ps(delta,delta),squared)),weight=_mm256_and_ps(_mm256_cmp_ps(delta,limit,_CMP_LT_OQ),_mm256_div_ps(gain,_mm256_mul_ps(gate,gate)));
        for(int c=0;c<Channels;++c){const auto center=_mm256_loadu_ps(data[c]+at);_mm256_storeu_ps(data[c]+at,_mm256_add_ps(center,_mm256_mul_ps(weight,_mm256_sub_ps(_mm256_loadu_ps(data[c]+neighbor),center))));}
    };
    for(int y=1;y<height;++y)for(int x=begin;x<end;x+=8)update(y,y-1,x);
    for(int y=height-2;y>=0;--y)for(int x=begin;x<end;x+=8)update(y,y+1,x);
}
void recursiveLumaVerticalAvx2(float* data,const float* original,int stride,int width,int height,int begin,int end,float range,float alpha){
    float* planes[]={data};const float* sources[]={original};recursiveVerticalCleanupAvx2<1>(planes,sources,nullptr,stride,width,height,begin,end,range,alpha);
}
void recursiveChromaVerticalAvx2(float* u,float* v,const float* originalU,const float* originalV,const float* guide,int stride,int width,int height,int begin,int end,float range,float alpha){
    float* planes[]={u,v};const float* sources[]={originalU,originalV};recursiveVerticalCleanupAvx2<2>(planes,sources,guide,stride,width,height,begin,end,range,alpha);
}
void recursiveChromaHorizontalAvx2(float* u,float* v,const float* originalU,const float* originalV,const float* guide,int stride,int width,int first,float range,float alpha){
    thread_local std::array<std::vector<float>,5> cache;for(auto& row:cache)row.resize(size_t(width)*8);
    float* sources[]={cache[0].data(),cache[1].data(),cache[2].data()},*filtered[]={cache[3].data(),cache[4].data()};
    const float* inputs[]={originalU,originalV,guide};
    for(int c=0;c<(guide?3:2);++c){const int pitch=stride;int x=0;for(;x+8<=width;x+=8){__m256 rows[8];for(int r=0;r<8;++r)rows[r]=_mm256_loadu_ps(inputs[c]+size_t(first+r)*pitch+x);transposeCleanup8(rows);for(int col=0;col<8;++col)_mm256_storeu_ps(sources[c]+size_t(x+col)*8,rows[col]);}for(;x<width;++x)for(int r=0;r<8;++r)sources[c][size_t(x)*8+r]=inputs[c][size_t(first+r)*pitch+x];}
    const auto sign=_mm256_set1_ps(-0.f),one=_mm256_set1_ps(1),half=_mm256_set1_ps(.5f),squared=_mm256_set1_ps(range*range),gain=_mm256_set1_ps(alpha),limit=_mm256_set1_ps(4*range);
    auto weight=[&](int x,int adjacent){auto delta=_mm256_setzero_ps();for(int c=0;c<2;++c){const auto difference=_mm256_andnot_ps(sign,_mm256_sub_ps(_mm256_loadu_ps(sources[c]+size_t(x)*8),_mm256_loadu_ps(sources[c]+size_t(adjacent)*8)));delta=c?_mm256_max_ps(delta,difference):difference;}const auto dg=guide?_mm256_mul_ps(half,_mm256_andnot_ps(sign,_mm256_sub_ps(_mm256_loadu_ps(sources[2]+size_t(x)*8),_mm256_loadu_ps(sources[2]+size_t(adjacent)*8)))):_mm256_setzero_ps();delta=_mm256_max_ps(delta,dg);const auto gate=_mm256_add_ps(one,_mm256_div_ps(_mm256_mul_ps(delta,delta),squared));return _mm256_and_ps(_mm256_cmp_ps(delta,limit,_CMP_LT_OQ),_mm256_div_ps(gain,_mm256_mul_ps(gate,gate)));};
    __m256 previous[2];for(int c=0;c<2;++c){previous[c]=_mm256_loadu_ps(sources[c]);_mm256_storeu_ps(filtered[c],previous[c]);}
    for(int x=1;x<width;++x){const auto a=weight(x,x-1);for(int c=0;c<2;++c){const auto center=_mm256_loadu_ps(sources[c]+size_t(x)*8);previous[c]=_mm256_add_ps(center,_mm256_mul_ps(a,_mm256_sub_ps(previous[c],center)));_mm256_storeu_ps(filtered[c]+size_t(x)*8,previous[c]);}}
    for(int x=width-2;x>=0;--x){const auto a=weight(x,x+1);for(int c=0;c<2;++c){const auto center=_mm256_loadu_ps(filtered[c]+size_t(x)*8);previous[c]=_mm256_add_ps(center,_mm256_mul_ps(a,_mm256_sub_ps(previous[c],center)));_mm256_storeu_ps(filtered[c]+size_t(x)*8,previous[c]);}}
    float* outputs[]={u,v};for(int c=0;c<2;++c){int x=0;for(;x+8<=width;x+=8){__m256 cols[8];for(int col=0;col<8;++col)cols[col]=_mm256_loadu_ps(filtered[c]+size_t(x+col)*8);transposeCleanup8(cols);for(int r=0;r<8;++r)_mm256_storeu_ps(outputs[c]+size_t(first+r)*stride+x,cols[r]);}for(;x<width;++x)for(int r=0;r<8;++r)outputs[c][size_t(first+r)*stride+x]=filtered[c][size_t(x)*8+r];}
}
}
