#pragma once
namespace fajk {
inline constexpr char gpuComputeShaders[] = R"HLSL(
cbuffer Parameters : register(b0) {
    uint4 plane0; uint4 plane1; uint4 plane2; // width,height,offset,padding
    uint level, inverse, planeCount, unused;
    uint4 image; // visible width,height,chroma (0/1/2),smoothing
    uint4 restoration; // guide threshold,CfL tile,columns,rows
    uint4 options; // coarse cleanup,native strength,luma strength,filtered planes
};
StructuredBuffer<float> input : register(t0);
RWStructuredBuffer<float> output : register(u0);
// 16 independent lines, 64 low/high pairs plus 4-pair halo on either side.
// Four barriers preserve lifting order; the halo covers all four stages.
groupshared float low[1152];
groupshared float high[1152];
static const float A=-1.586134342f, B=-.05298011854f, C=.8829110762f, D=.4435068522f, N=1.149604398f;
void transform(uint3 group, uint3 tid, bool vertical, bool reverse) {
    uint4 p=group.z==0?plane0:group.z==1?plane1:plane2;
    uint width=p.x>>level, height=p.y>>level;
    uint lineIndex=(vertical?group.x:group.y)*16+(vertical?tid.x:tid.y);
    int pairCount=int((vertical?height:width)/2);
    int begin=int((vertical?group.y:group.x)*64)-4;
    if(group.z>=planeCount || (vertical?group.x*16>=width:group.y*16>=height) || begin+4>=pairCount)return;
    uint localLine=vertical?tid.x:tid.y;
    uint lane=vertical?tid.y:tid.x;
    uint base=localLine*72;
    uint lineBound=vertical?width:height;
    for(uint j=lane;j<72;j+=16) {
        uint n=uint(clamp(begin+int(j),0,pairCount-1));
        uint a=reverse?n:2*n, b=reverse?n+pairCount:2*n+1;
        uint ix=p.z+(vertical?a*p.x+min(lineIndex,lineBound-1):min(lineIndex,lineBound-1)*p.x+a);
        uint iy=p.z+(vertical?b*p.x+min(lineIndex,lineBound-1):min(lineIndex,lineBound-1)*p.x+b);
        low[base+j]=reverse?input[ix]*(1.f/N):input[ix];
        high[base+j]=reverse?input[iy]*N:input[iy];
    }
    GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint step=0;step<4;++step) {
        bool updateLow=reverse?(step%2==0):(step%2!=0);
        float factor=reverse?(step==0?-D:step==1?-C:step==2?-B:-A):(step==0?A:step==1?B:step==2?C:D);
        // Updates read the other array only; no thread reads an array being written.
        for(uint j=lane;j<72;j+=16) {
            int n=clamp(begin+int(j),0,pairCount-1);
            uint own=uint(clamp(n-begin,0,71));
            uint neighbor=uint(clamp(clamp(n+(updateLow?-1:1),0,pairCount-1)-begin,0,71));
            if(updateLow) {
                precise float sum=high[base+own]+high[base+neighbor];
                precise float value=low[base+j]+factor*sum;
                low[base+j]=value;
            }else {
                precise float sum=low[base+own]+low[base+neighbor];
                precise float value=high[base+j]+factor*sum;
                high[base+j]=value;
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if(lineIndex>=lineBound)return;
    for(uint j=lane;j<64;j+=16) {
        uint n=uint(begin+4)+j;
        if(n>=uint(pairCount))continue;
        uint a=reverse?2*n:n, b=reverse?2*n+1:n+pairCount;
        uint ix=p.z+(vertical?a*p.x+lineIndex:lineIndex*p.x+a);
        uint iy=p.z+(vertical?b*p.x+lineIndex:lineIndex*p.x+b);
        output[ix]=reverse?low[base+j+4]:low[base+j+4]*N;
        // CPU first rounds N to float, then rounds its reciprocal. FXC folds
        // 1/N with extra precision (one ULP higher), changing encode decisions.
        output[iy]=reverse?high[base+j+4]:high[base+j+4]*asfloat(0x3f5eaf6fu);
    }
}
[numthreads(16,16,1)]void horizontal(uint3 g:SV_GroupID,uint3 t:SV_GroupThreadID){transform(g,t,false,false);}
[numthreads(16,16,1)]void vertical(uint3 g:SV_GroupID,uint3 t:SV_GroupThreadID){transform(g,t,true,false);}

[numthreads(16,16,1)]void inverseHorizontal(uint3 g:SV_GroupID,uint3 t:SV_GroupThreadID){transform(g,t,false,true);}
[numthreads(16,16,1)]void inverseVertical(uint3 g:SV_GroupID,uint3 t:SV_GroupThreadID){transform(g,t,true,true);}

// Positive antialiased triangle resampler (production smooth=2). CPU supplies
// the exact normalized taps, avoiding GPU sin/division/coordinate differences.
StructuredBuffer<uint> rgba : register(t1);
struct Tap {uint first,count;};
StructuredBuffer<Tap> taps : register(t2);
struct Weight {uint index;float value;};
StructuredBuffer<Weight> weights : register(t3);
StructuredBuffer<float4> intermediate : register(t4);
RWStructuredBuffer<float4> temporary : register(u1);
RWStructuredBuffer<uint> packed : register(u2);
[numthreads(16,16,1)]void resampleH(uint3 id:SV_DispatchThreadID) {
    uint iw=plane0.x,ih=plane0.y,ow=plane1.x;
    if(id.x>=ow||id.y>=ih)return;
    Tap t=taps[id.x];precise float4 sum=0;
    for(uint j=0;j<t.count;++j){Weight v=weights[t.first+j];uint c=rgba[id.y*iw+v.index];
        float4 color=float4(c&255,(c>>8)&255,(c>>16)&255,c>>24);
        sum=sum+color*v.value;
    }
    temporary[id.y*ow+id.x]=sum;
}
[numthreads(16,16,1)]void resampleV(uint3 id:SV_DispatchThreadID) {
    uint ow=plane1.x,oh=plane1.y;
    if(id.x>=ow||id.y>=oh)return;
    Tap t=taps[ow+id.y];precise float4 sum=0;
    for(uint j=0;j<t.count;++j){Weight v=weights[t.first+j];sum=sum+intermediate[v.index*ow+id.x]*v.value;}
    uint3 c=uint3(clamp(sum.rgb,0,255)+.5f);
    packed[id.y*ow+id.x]=c.x|(c.y<<8)|(c.z<<16)|0xff000000;
}
struct SourceStatistics {
    uint xx4,yy4;int xy4;uint3 sum;int3 sumX2,sumY2;uint3 square;
    uint quiet,strong,opaque,pad0,pad1;
};
RWStructuredBuffer<SourceStatistics> statistics : register(u3);
int3 rgb(uint x,uint y){uint v=rgba[y*image.x+x];return int3(v&255,(v>>8)&255,(v>>16)&255);}
[numthreads(8,8,1)]void sourceAnalysis(uint3 id:SV_DispatchThreadID){
    uint cell=restoration.x,cols=restoration.z,rows=restoration.w;
    if(id.x>=cols||id.y>=rows)return;
    uint x0=id.x*cell,x1=min(image.x,x0+cell),y0=id.y*cell,y1=min(image.y,y0+cell);
    SourceStatistics s=(SourceStatistics)0;s.opaque=1;
    for(uint y=y0;y<y1;++y)for(uint x=x0;x<x1;++x){
        int3 p=rgb(x,y),dx=rgb(min(image.x-1,x+1),y)-rgb(x>0?x-1:0,y);
        int3 dy=rgb(x,min(image.y-1,y+1))-rgb(x,y>0?y-1:0);
        s.xx4+=dot(dx,dx);s.yy4+=dot(dy,dy);s.xy4+=dot(dx,dy);
        uint gradient=dot(dx,dx)+dot(dy,dy);
        s.quiet+=gradient<=(level>=30?64:16);s.strong+=gradient>=(level>=28?576:4096);
        s.sum+=p;s.sumX2+=p*(int(2*x)-int(x0+x1-1));s.sumY2+=p*(int(2*y)-int(y0+y1-1));s.square+=p*p;
        uint a=rgba[y*image.x+x]&rgba[y*image.x+(x>0?x-1:0)]&rgba[y*image.x+min(image.x-1,x+1)]&rgba[(y>0?y-1:0)*image.x+x]&rgba[min(image.y-1,y+1)*image.x+x];
        s.opaque&=(a>>24)==255;
    }
    statistics[id.y*cols+id.x]=s;
}

StructuredBuffer<float> gammaTable : register(t5);
StructuredBuffer<uint> slopes : register(t6);
StructuredBuffer<float> means : register(t7);
StructuredBuffer<float> guides : register(t8);
RWStructuredBuffer<float> meanOutput : register(u3);
RWStructuredBuffer<float> guideOutput : register(u4);
groupshared float meanPartial[64];
[numthreads(64,1,1)]void cellMeans(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint tile=restoration.y,x0=group.x*tile,y0=group.y*tile;
    precise float sum=0;
    for(uint i=lane;i<tile*tile;i+=64){uint x=x0+i%tile,y=y0+i/tile;if(x<image.x&&y<image.y)sum+=input[y*plane0.x+x];}
    meanPartial[lane]=sum;GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint d=32;d>0;d>>=1){if(lane<d)meanPartial[lane]+=meanPartial[lane+d];GroupMemoryBarrierWithGroupSync();}
    if(lane==0)meanOutput[group.y*restoration.z+group.x]=meanPartial[0]/float((min(image.x,x0+tile)-x0)*(min(image.y,y0+tile)-y0));
}
[numthreads(16,16,1)]void chromaGuides(uint3 id:SV_DispatchThreadID){
    if(id.x>=(image.x+1)/2||id.y>=(image.y+1)/2)return;
    uint x=id.x*2,y=id.y*2,nx=min(image.x-1,x+1),ny=min(image.y-1,y+1);
    precise float value=((input[y*plane0.x+x]+input[y*plane0.x+nx])+input[ny*plane0.x+x])+input[ny*plane0.x+nx];
    guideOutput[id.y*plane1.x+id.x]=value*.25f;
}
float3 control(int x,int y0,int y1,float f){
    uint a=uint(y0)*restoration.z+uint(clamp(x,0,int(restoration.z)-1)),b=uint(y1)*restoration.z+uint(clamp(x,0,int(restoration.z)-1));
    uint sa=slopes[a],sb=slopes[b];
    float2 va=float2(int(sa<<24)>>24,int(sa<<16)>>24),vb=float2(int(sb<<24)>>24,int(sb<<16)>>24);
    precise float2 slope=(va*(1-f)+vb*f)/16.f;
    precise float mean=means[a]*(1-f)+means[b]*f;
    return float3(slope,mean);
}
float gammaByte(float v){v=clamp(v,0.f,1.f)*8192;uint at=uint(v);precise float value=gammaTable[at]+(v-at)*(gammaTable[min(8192,at+1)]-gammaTable[at]);return value;}
[numthreads(16,16,1)]void nativeColor(uint3 id:SV_DispatchThreadID){
    uint x=id.x,y=id.y;if(x>=image.x||y>=image.y)return;
    precise float light=input[y*plane0.x+x];
    precise float2 color=0;
    uint cx=x/2,cy=y/2;
    if(image.z==1&&restoration.x>0){
        uint nx=uint(clamp(int(cx)+(x%2?1:-1),0,int((image.x+1)/2)-1));
        uint ny=uint(clamp(int(cy)+(y%2?1:-1),0,int((image.y+1)/2)-1));
        uint4 pos=uint4(cy*plane1.x+cx,cy*plane1.x+nx,ny*plane1.x+cx,ny*plane1.x+nx);
        float4 basis=float4(.5625f,.1875f,.1875f,.0625f);precise float total=0;
        [unroll]for(uint i=0;i<4;++i){precise float d=light-guides[pos[i]];
            precise float weight=basis[i]/(1+d*d*(1.f/(restoration.x*restoration.x)));
            total+=weight;color+=weight*float2(input[plane1.z+pos[i]],input[plane2.z+pos[i]]);}
        color/=total;
    }else if(image.z!=0){
        uint by=image.z==1?cy:y;
        uint nx=uint(clamp(int(cx)+(x%2?1:-1),0,int(plane1.x)-1));
        uint ny=image.z==1?uint(clamp(int(cy)+(y%2?1:-1),0,int(plane1.y)-1)):y;
        float n=image.z==2?(image.w>0?.25f:0):.25f*image.w/100.f;
        [unroll]for(uint c=0;c<2;++c){uint offset=c==0?plane1.z:plane2.z;
            precise float upper=input[offset+by*plane1.x+cx]*(1-n)+input[offset+by*plane1.x+nx]*n;
            precise float lower=input[offset+ny*plane1.x+cx]*(1-n)+input[offset+ny*plane1.x+nx]*n;
            color[c]=image.z==2?upper:upper*(1-n)+lower*n;}
    }else color=float2(input[plane1.z+y*plane1.x+x],input[plane2.z+y*plane2.x+x]);
    if(restoration.y>0){
        float tile=restoration.y,center=(tile-1)*.5f,gy=(float(y)-center)/tile;int iy=int(floor(gy));float fy=gy-iy;
        int u=clamp(iy,0,int(restoration.w)-1),v=clamp(iy+1,0,int(restoration.w)-1);
        float gx=(float(x)-center)/tile;int ix=int(floor(gx));uint begin=uint(max(0.f,ceil(ix*tile+center)));
        float3 a=control(ix,u,v,fy),b=control(ix+1,u,v,fy);
        precise float3 initial=a+(b-a)*((float(begin)-center)/tile-ix),delta=(b-a)/tile;
        precise float3 model=initial+delta*float(x-begin);
        color+=model.xy*(light-model.z);
    }
    precise float L=(light+128.f)*(1.f/255),a=color.x*(1.f/384),b=color.y*(1.f/384);
    precise float l=(L+.3963377774f*a)+.2158037573f*b;
    precise float m=(L-.1055613458f*a)-.0638541728f*b;
    precise float s=(L-.0894841775f*a)-1.2914855480f*b;
    l=l*(l*l);m=m*(m*m);s=s*(s*s);
    precise float r=(4.0767416621f*l-3.3077115913f*m)+.2309699292f*s;
    precise float g=(-1.2684380046f*l+2.6097574011f*m)-.3413193965f*s;
    precise float blue=(-.0041960863f*l-.7034186147f*m)+1.7076147010f*s;
    uint3 value=uint3(gammaByte(r)+.5f,gammaByte(g)+.5f,gammaByte(blue)+.5f);
    packed[y*image.x+x]=value.x|(value.y<<8)|(value.z<<16)|0xff000000;
}
float root(float x){
    precise float scaled=float(asuint(x))*(1.f/3);
    float y=asfloat(uint(scaled)+709921077);
    [unroll]for(uint i=0;i<2;++i){precise float value=((y+y)+x/(y*y))*(1.f/3);y=value;}
    return x>0?y:0;
}
[numthreads(16,16,1)]void sourceColor(uint3 id:SV_DispatchThreadID){
    if(id.x>=plane0.x||id.y>=plane0.y)return;
    uint x=min(id.x,image.x-1),y=min(id.y,image.y-1),v=rgba[y*image.x+x];
    float r=gammaTable[v&255],g=gammaTable[(v>>8)&255],b=gammaTable[(v>>16)&255];
    precise float l=root((.4122214708f*r+.5363325363f*g)+.0514459929f*b);
    precise float m=root((.2119034982f*r+.6806995451f*g)+.1073969566f*b);
    precise float s=root((.0883024619f*r+.2817188376f*g)+.6299787005f*b);
    precise float L=255*((.2104542553f*l+.7936177850f*m)-.0040720468f*s)-128;
    precise float A=384*((1.9779984951f*l-2.4285922050f*m)+.4505937099f*s);
    precise float B=384*((.0259040371f*l+.7827717662f*m)-.8086757660f*s);
    uint at=id.y*plane0.x+id.x;output[at]=L;output[plane1.z+at]=A;output[plane2.z+at]=B;
}
// Source color planes are three full-resolution padded planes. The prepared
// output uses the target plane offsets, with independently padded 4:2:0 chroma.
[numthreads(16,16,1)]void sourceWavelet(uint3 id:SV_DispatchThreadID){
    uint x=id.x,y=id.y,area=plane0.x*plane0.y;
    if(x<plane0.x&&y<plane0.y)output[y*plane0.x+x]=input[y*plane0.x+x];
    if(x>=plane1.x||y>=plane1.y)return;
    uint ax=min(image.x-1,x*2),bx=min(image.x-1,ax+1),ay=min(image.y-1,y*2),by=min(image.y-1,ay+1);
    uint4 at=uint4(ay*plane0.x+ax,ay*plane0.x+bx,by*plane0.x+ax,by*plane0.x+bx);
    precise float ac=((input[at.x]+input[at.y])+(input[at.z]+input[at.w]))*.25f;
    float tile=restoration.y,center=(tile-1)*.5f;
    precise float gy=(float(ay)+.5f-center)/tile;int iy=int(floor(gy));float fy=gy-iy;
    int u=clamp(iy,0,int(restoration.w)-1),v=clamp(iy+1,0,int(restoration.w)-1);
    precise float gx=(float(x*2)+.5f-center)/tile;int ix=int(floor(gx));
    uint begin=uint(max(0.f,ceil((ix*tile+center-.5f)*.5f)));
    precise float start=(float(begin*2)+.5f-center)/tile-float(ix);
    float3 a=control(ix,u,v,fy),b=control(ix+1,u,v,fy);
    precise float3 initial=a+(b-a)*start,delta=((b-a)*2.f)/tile;
    precise float3 model=initial+delta*float(x-begin);
    ac=ac-model.z;
    [unroll]for(uint c=0;c<2;++c){
        uint off=(c+1)*area;
        precise float average=((input[off+at.x]+input[off+at.y])+(input[off+at.z]+input[off+at.w]))*.25f;
        output[(c==0?plane1.z:plane2.z)+y*plane1.x+x]=average-model[c]*ac;
    }
}
float2 coarseValue(uint at,bool chroma){return float2(input[(chroma?plane1.z:plane0.z)+at],chroma?input[plane2.z+at]:0);}
float recursiveWeight(float2 a,float2 b,bool chroma){
    uint code=options.x;
    uint mode=chroma?(code>=12?code-11:code-4):(code>=8?5:code);
    static const float lr[7]={8,16,32,4,6,8,8},la[7]={.6f,.75f,.85f,.6f,.5f,.35f,.8f};
    static const float cr[7]={4,8,16,2,4,6,8},ca[7]={.6f,.75f,.85f,.5f,.5f,.5f,.5f};
    float range=chroma?cr[mode-1]:lr[mode-1],alpha=chroma?ca[mode-1]:la[mode-1];
    float2 delta=abs(a-b);float d=chroma?max(delta.x,delta.y):delta.x;
    precise float gate=1+d*d/(range*range);return d>=4*range?0:alpha/(gate*gate);
}
void coarseFilter(uint3 id,bool vertical){
    bool chroma=id.z==1;if(chroma&&options.x<8)return;
    uint4 p=chroma?plane1:plane0;uint w=p.x/2,h=p.y/2;
    uint lineIndex=id.x,length=vertical?h:w;if(lineIndex>=(vertical?w:h))return;
    uint step=vertical?p.x:1,begin=vertical?lineIndex:lineIndex*p.x;
    precise float2 value=coarseValue(begin,chroma);output[p.z+begin]=value.x;if(chroma)output[plane2.z+begin]=value.y;
    for(uint i=1;i<length;++i){uint at=begin+i*step;float2 own=coarseValue(at,chroma),previous=coarseValue(at-step,chroma);
        precise float weight=recursiveWeight(own,previous,chroma);value=own+weight*(value-own);
        output[p.z+at]=value.x;if(chroma)output[plane2.z+at]=value.y;
    }
    for(int i=int(length)-2;i>=0;--i){uint at=begin+uint(i)*step;
        float2 own=float2(output[p.z+at],chroma?output[plane2.z+at]:0);
        precise float weight=recursiveWeight(coarseValue(at,chroma),coarseValue(at+step,chroma),chroma);value=own+weight*(value-own);
        output[p.z+at]=value.x;if(chroma)output[plane2.z+at]=value.y;
    }
}
[numthreads(64,1,1)]void coarseH(uint3 id:SV_DispatchThreadID){coarseFilter(id,false);}
[numthreads(64,1,1)]void coarseV(uint3 id:SV_DispatchThreadID){coarseFilter(id,true);}
groupshared uint nativeDirection;
float constrained(float d,float limit){float magnitude=abs(d);float v=min(magnitude,max(0.f,limit-magnitude*.25f));return asfloat(asuint(v)|(asuint(d)&0x80000000));}
[numthreads(8,8,1)]void nativeRestore(uint3 group:SV_GroupID,uint3 tid:SV_GroupThreadID){
    uint4 p=group.z==0?plane0:group.z==1?plane1:plane2;
    uint width=group.z>0&&image.z>0?(image.x+1)/2:image.x,height=group.z>0&&image.z==1?(image.y+1)/2:image.y;
    uint x=group.x*8+tid.x,y=group.y*8+tid.y;
    float strength=group.z==0?float(options.z):float(options.y)*.5f;
    bool active=width>=8&&height>=8&&strength>0&&(options.w&(group.z>0?2:1));
    int offsets[4]={1,int(p.x),int(p.x)+1,int(p.x)-1};
    int left=max(2,int(group.x*8)),right=min(int(width)-2,int(group.x*8+8));
    int top=max(2,int(group.y*8)),bottom=min(int(height)-2,int(group.y*8+8));
    if(tid.x==0&&tid.y==0){nativeDirection=0;
        if(active&&right>left&&bottom>top){float best=3.402823466e+38f;
            [unroll]for(uint d=0;d<4;++d){precise float4 energy=0;precise float tail=0;
                for(int yy=top;yy<bottom;yy+=2){int xx=left;
                    for(;xx+4<=right;xx+=4){[unroll]for(uint lane=0;lane<4;++lane){int at=int(p.z)+yy*int(p.x)+xx+lane;float delta=input[at+offsets[d]]-input[at-offsets[d]];energy[lane]+=delta*delta;}}
                    for(;xx<right;++xx){int at=int(p.z)+yy*int(p.x)+xx;float delta=input[at+offsets[d]]-input[at-offsets[d]];tail+=delta*delta;}}
                precise float cost=((energy.x+energy.y)+(energy.z+energy.w))+tail;
                if(cost<best){best=cost;nativeDirection=d;}
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if(x>=p.x||y>=p.y)return;
    uint at=p.z+y*p.x+x;precise float center=input[at],sum=0;
    if(active&&x>=2&&y>=2&&x<width-2&&y<height-2){int primary=offsets[nativeDirection],secondary=offsets[(nativeDirection+2)%4];
        [unroll]for(int side=-1;side<=1;side+=2){sum+=constrained(input[int(at)+side*primary]-center,strength)*(3.f/16);
            sum+=constrained(input[int(at)+side*2*primary]-center,strength)*(2.f/16);
            sum+=constrained(input[int(at)+side*secondary]-center,strength*.5f)*(1.f/16);}
    }
    output[at]=center+sum;
}
// R470 post-contour filter. Each line has a single owner throughout both
// recursive sweeps. Tile geometry/padded endpoints match the AVX2 route.
float3 contourRgb(uint p){return float3(p&255,(p>>8)&255,(p>>16)&255);}
uint contourRaw(uint2 p){return rgba[p.y*image.x+p.x];}
float contourLambda(uint a,uint b){
    float3 d=abs(contourRgb(a)-contourRgb(b));
    precise float r=max(d.x,max(d.y,d.z))*asfloat(restoration.w);
    precise float curve=max(0.f,1.f-r*r);
    return ((a&b)>>24)==255?.78f*(curve*curve):0;
}
float3 contourStep(float3 own,float weight,float3 previous){
    if(options.z)return mad(weight,previous-own,own);
    precise float3 result=own+weight*(previous-own);return result;
}
void contourTile(uint tileId,out uint2 origin,out uint2 extent,out uint2 pitch){
    uint2 begin=uint2(tileId%options.y,tileId/options.y)*96;
    origin=uint2(max(0,int(begin.x)-8),max(0,int(begin.y)-8));
    extent=min(image.xy,begin+104)-origin;
    pitch=(extent+7)&~7u;
}
bool contourSkip(uint tileId){return options.w&&slopes[tileId]!=0;}
groupshared float3 contourValues[1792];
groupshared uint contourWeights[1792];
[numthreads(16,1,1)]void contourH(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    if(contourSkip(group.x))return;
    uint2 origin,extent,pitch;contourTile(group.x,origin,extent,pitch);
    uint stripe=group.y*16;if(stripe>=pitch.y)return;
    for(uint j=lane;j<16*pitch.x;j+=16){uint x=j%pitch.x,y=j/pitch.x;
        uint raw=contourRaw(origin+uint2(min(x,extent.x-1),min(stripe+y,extent.y-1)));
        contourWeights[x*16+y]=raw;contourValues[x*16+y]=contourRgb(raw);
    }
    GroupMemoryBarrierWithGroupSync();
    uint previous=contourWeights[lane];precise float3 value=contourValues[lane];
    for(uint x=1;x<pitch.x;++x){uint at=x*16+lane,raw=contourWeights[at];float weight=contourLambda(raw,previous);previous=raw;
        value=contourStep(contourValues[at],weight,value);contourValues[at]=value;contourWeights[at]=asuint(weight);
    }
    for(int x=int(pitch.x)-2;x>=0;--x){uint at=uint(x)*16+lane;
        value=contourStep(contourValues[at],asfloat(contourWeights[at+16]),value);contourValues[at]=value;
    }
    GroupMemoryBarrierWithGroupSync();
    for(uint j=lane;j<16*pitch.x;j+=16){uint x=j%pitch.x,y=j/pitch.x;
        if(stripe+y<pitch.y)temporary[group.x*12544+(stripe+y)*112+x]=float4(contourValues[x*16+y],0);
    }
}
[numthreads(16,1,1)]void contourV(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    if(contourSkip(group.x))return;
    uint2 origin,extent,pitch;contourTile(group.x,origin,extent,pitch);
    uint stripe=group.y*16;if(stripe>=pitch.x)return;
    for(uint y=0;y<pitch.y;++y){uint x=min(stripe+lane,pitch.x-1),at=y*16+lane;
        contourWeights[at]=contourRaw(origin+uint2(min(x,extent.x-1),min(y,extent.y-1)));
        contourValues[at]=temporary[group.x*12544+y*112+x].xyz;
    }
    GroupMemoryBarrierWithGroupSync();
    uint previous=contourWeights[lane];precise float3 value=contourValues[lane];
    for(uint y=1;y<pitch.y;++y){uint at=y*16+lane,raw=contourWeights[at];float weight=contourLambda(raw,previous);previous=raw;
        value=contourStep(contourValues[at],weight,value);contourValues[at]=value;contourWeights[at]=asuint(weight);
    }
    for(int y=int(pitch.y)-2;y>=0;--y){uint at=uint(y)*16+lane;
        value=contourStep(contourValues[at],asfloat(contourWeights[at+16]),value);contourValues[at]=value;
    }
    GroupMemoryBarrierWithGroupSync();
    if(stripe+lane<pitch.x)for(uint y=0;y<pitch.y;++y)temporary[group.x*12544+y*112+stripe+lane]=float4(contourValues[y*16+lane],0);
}
float3 contourAt(uint tileId,int2 p,uint2 extent){
    p=clamp(p,int2(0,0),int2(extent)-1);
    return intermediate[tileId*12544+uint(p.y)*112+uint(p.x)].xyz;
}
uint contourPack(float3 value,uint original){
    uint3 v=uint3(clamp(value,0.f,255.f)+.5f);
    return (original&0xff000000)|v.x|(v.y<<8)|(v.z<<16);
}
[numthreads(16,16,1)]void contourOutput(uint3 id:SV_DispatchThreadID){
    if(any(id.xy>=image.xy))return;
    uint at=id.y*image.x+id.x,original=rgba[at];
    uint tileId=(id.y/96)*options.y+id.x/96;
    if(contourSkip(tileId)||(original>>24)!=255){packed[at]=original;return;}
    uint2 origin,extent,pitch;contourTile(tileId,origin,extent,pitch);int2 p=int2(id.xy-origin);
    precise float3 clean=contourAt(tileId,p,extent);
    float strength=asfloat(restoration.z);
    if(strength>0){
        precise float3 dx=contourAt(tileId,p+int2(1,0),extent)-contourAt(tileId,p-int2(1,0),extent);
        precise float3 dy=contourAt(tileId,p+int2(0,1),extent)-contourAt(tileId,p-int2(0,1),extent);
        precise float3 energies=dx*dx+dy*dy;
        float best=0,gx=0,gy=0;
        [unroll]for(uint c=0;c<3;++c)if(energies[c]>best){best=energies[c];gx=dx[c];gy=dy[c];}
        if(best>64){
            precise float squared=best+1,inverseLength=rsqrt(squared);
            inverseLength=inverseLength*(1.5f-(.5f*squared)*(inverseLength*inverseLength));
            precise float2 tangent=float2(-gy,gx)*inverseLength;
            precise float3 sum=0;precise float total=0;
            [unroll]for(int side=-1;side<=1;side+=2){
                precise float2 location=(float2(p)+tangent*float(side*2))+.5f;
                int2 tap=int2(floor(location));
                precise float3 delta=contourAt(tileId,tap,extent)-clean;
                float3 d=abs(delta);precise float weight=max(0.f,1.f-max(d.x,max(d.y,d.z))*(1.f/48));weight=weight*weight;
                total+=weight;sum+=weight*delta;
            }
            precise float active=clamp((best-64)*(1.f/512),0.f,1.f);
            precise float scale=(active*min(1.f,strength*2))/max(1.f,total);clean+=scale*sum;
        }
    }
    precise float3 own=contourRgb(original),value=own+asfloat(restoration.x)*(clean-own);
    packed[at]=contourPack(value,original);
}
[numthreads(16,16,1)]void contourWeak(uint3 id:SV_DispatchThreadID){
    if(any(id.xy>=image.xy))return;
    uint at=id.y*image.x+id.x,original=rgba[at];float3 own=contourRgb(original);
    precise float3 sum=0;precise float total=1;
    static const int2 offsets[4]={int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)};
    [unroll]for(uint i=0;i<4;++i){uint2 p=uint2(clamp(int2(id.xy)+offsets[i],int2(0,0),int2(image.xy)-1));uint tap=contourRaw(p);
        float3 d=abs(contourRgb(tap)-own);precise float t=max(d.x,max(d.y,d.z))*(1.f/32);
        t=max(0.f,1.f-t*t);t=t*t;if(((original&tap)>>24)!=255)t=0;
        total+=t;sum+=t*(contourRgb(tap)-own);
    }
    precise float scale=asfloat(restoration.x)/total;precise float3 value=own+scale*sum;
    packed[at]=contourPack(value,original);
}
struct ColorStatistics {
    double sum,square,color[2],cross[2];
    double detailVariance,detailCross[2];
    uint count,detailCount;
};
RWStructuredBuffer<ColorStatistics> fittedColor : register(u3);
double3 modelColor(uint x,uint y){
    uint at=y*plane0.x+x;
    return double3(input[plane0.z+at],input[plane1.z+at],input[plane2.z+at]);
}
groupshared ColorStatistics colorPartial[64];
// One cell per group with 64 independent sample accumulators. Double partial
// sums are reduced in a fixed tree; CPU quantization guards rounding borders.
[numthreads(64,1,1)]void colorModelStatistics(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint cols=restoration.z,tile=restoration.x;
    uint x0=(group.x%cols)*tile,y0=(group.x/cols)*tile;
    uint w=min(image.x,x0+tile)-x0,h=min(image.y,y0+tile)-y0;
    ColorStatistics s=(ColorStatistics)0;
    [loop]for(uint at=lane;at<w*h;at+=64){
        precise double3 v=modelColor(x0+at%w,y0+at/w);
        s.sum=s.sum+v.x;s.square=s.square+v.x*v.x;
        [unroll]for(uint c=0;c<2;++c){s.color[c]=s.color[c]+v[c+1];s.cross[c]=s.cross[c]+v.x*v[c+1];}
        ++s.count;
    }
    if(tile==64){
        uint bw=(w+1)/2,bh=(h+1)/2;
        [loop]for(uint at=lane;at<bw*bh;at+=64){
            uint x=x0+(at%bw)*2,y=y0+(at/bw)*2;
            double3 v[4]={modelColor(x,y),modelColor(min(image.x-1,x+1),y),
                modelColor(x,min(image.y-1,y+1)),modelColor(min(image.x-1,x+1),min(image.y-1,y+1))};
            precise double3 mean=0;
            [unroll]for(uint i=0;i<4;++i)mean=mean+v[i]*.25;
            [unroll]for(uint i=0;i<4;++i){
                precise double dy=v[i].x-mean.x;
                s.detailVariance=s.detailVariance+dy*dy;
                [unroll]for(uint c=0;c<2;++c)s.detailCross[c]=s.detailCross[c]+dy*(v[i][c+1]-mean[c+1]);
            }
            s.detailCount+=4;
        }
    }
    colorPartial[lane]=s;
    GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint stride=32;stride>0;stride>>=1){
        if(lane<stride){
            ColorStatistics b=colorPartial[lane+stride];
            s.sum=s.sum+b.sum;s.square=s.square+b.square;
            [unroll]for(uint c=0;c<2;++c){s.color[c]=s.color[c]+b.color[c];s.cross[c]=s.cross[c]+b.cross[c];s.detailCross[c]=s.detailCross[c]+b.detailCross[c];}
            s.detailVariance=s.detailVariance+b.detailVariance;s.count+=b.count;s.detailCount+=b.detailCount;
            colorPartial[lane]=s;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane==0)fittedColor[group.x]=s;
}
// Each 2x2 moment is computed once, then reused by all patch equations.
RWStructuredBuffer<double4> momentOutput : register(u1);
StructuredBuffer<double4> moments : register(t4);
[numthreads(16,16,1)]void colorDetailMoments(uint3 id:SV_DispatchThreadID){
    uint2 count=(image.xy+1)/2;
    if(any(id.xy>=count))return;
    uint x=id.x*2,y=id.y*2;
    precise double3 sum=0;precise double yy=0;precise double2 yc=0;
    [unroll]for(uint j=0;j<2;++j)[unroll]for(uint i=0;i<2;++i){
        precise double3 v=modelColor(min(image.x-1,x+i),min(image.y-1,y+j));
        sum=sum+v;yy=yy+v.x*v.x;yc=yc+v.x*v.yz;
    }
    precise double variance=max(double(0),yy-(sum.x*sum.x)*.25);
    precise double2 covariance=yc-(sum.x*sum.yz)*.25;
    momentOutput[id.y*count.x+id.x]=double4(variance,covariance,0);
}
struct ColorPatch {double covariance[4][2],gram[4][4],samples[4];};
RWStructuredBuffer<ColorPatch> fittedPatches : register(u3);
groupshared double patchSums[28];
// One warp per patch, one independent scalar equation per lane. Every sum
// visits samples in the original order; all lanes reuse cached 2x2 moments.
[numthreads(32,1,1)]void colorModelPatches(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint cols=restoration.z,tile=restoration.x;
    uint px=group.x%(cols+1),py=group.x/(cols+1);
    int ix=int(px)-1,iy=int(py)-1;
    uint x0=uint(max(0,int(px*tile)-int(tile/2))),y0=uint(max(0,int(py*tile)-int(tile/2)));
    uint x1=min(image.x,px*tile+tile/2),y1=min(image.y,py*tile+tile/2);
    float center=(tile-1)*.5f;
    precise double total=0;
    if(lane<28){
        uint a=lane<8?lane/2:lane<24?(lane-8)/4:lane-24;
        uint b=lane<24?(lane-8)%4:0;
        if(lane<8||lane>=24||a<=b){
            [loop]for(uint y=y0;y<y1;y+=2){
                precise float fy=(float(y)+.5f-center)/float(tile)-float(iy);
                [loop]for(uint x=x0;x<x1;x+=2){
                    precise float fx=(float(x)+.5f-center)/float(tile)-float(ix);
                    precise float wa=(a%2?fx:1-fx)*(a/2?fy:1-fy);
                    precise float wb=(b%2?fx:1-fx)*(b/2?fy:1-fy);
                    double4 v=moments[(y/2)*((image.x+1)/2)+x/2];
                    precise double contribution=lane<8?double(wa)*(lane%2?v.z:v.y):lane<24?(double(wa)*double(wb))*v.x:double(wa);
                    total=total+contribution;
                }
            }
        }
        patchSums[lane]=total;
    }
    GroupMemoryBarrierWithGroupSync();
    if(lane==0){
        ColorPatch p=(ColorPatch)0;
        [unroll]for(uint i=0;i<4;++i){
            [unroll]for(uint c=0;c<2;++c)p.covariance[i][c]=patchSums[i*2+c];
            [unroll]for(uint j=i;j<4;++j)p.gram[i][j]=patchSums[8+i*4+j];
            p.samples[i]=patchSums[24+i];
        }
        fittedPatches[group.x]=p;
    }
}
// Production fast edge resampler. Coordinates come from the same double CPU
// axis builder. Eight-lane activation matches the original AVX2 row, including
// its final replicated vector; no texture sampler or hardware interpolation.
groupshared uint upscaleActive[256];
uint upscaleRaw(int2 p){
    p=clamp(p,int2(0,0),int2(plane0.xy)-1);
    return rgba[uint(p.y)*plane0.x+uint(p.x)];
}
[numthreads(32,8,1)]void edgeUpscale(uint3 id:SV_DispatchThreadID,uint3 tid:SV_GroupThreadID){
    uint2 pos=min(id.xy,image.xy-1);
    Tap ax=taps[pos.x],ay=taps[image.x+pos.y];
    int2 origin=int2(asint(ax.first),asint(ay.first));
    float fx=asfloat(ax.count),fy=asfloat(ay.count),amount=asfloat(restoration.x);
    uint a=upscaleRaw(origin),b=upscaleRaw(origin+int2(1,0));
    uint c=upscaleRaw(origin+int2(0,1)),d=upscaleRaw(origin+int2(1,1));
    float3 av=contourRgb(a),bv=contourRgb(b),cv=contourRgb(c),dv=contourRgb(d);
    precise float3 upper=av+(bv-av)*fx,lower=cv+(dv-cv)*fx;
    precise float3 base=upper+(lower-upper)*fy;
    float3 minimum=min(min(av,bv),min(cv,dv)),maximum=max(max(av,bv),max(cv,dv));
    float3 spans=maximum-minimum;
    float best=spans.x;uint shift=0;
    if(spans.y>best){best=spans.y;shift=8;}
    if(spans.z>best){best=spans.z;shift=16;}
    uint lane=tid.y*32+tid.x;
    upscaleActive[lane]=best>3;
    GroupMemoryBarrierWithGroupSync();
    uint active=0,first=lane&~7u;
    [unroll]for(uint i=0;i<8;++i)active|=upscaleActive[first+i];
    if(amount>0&&active){
        uint corners[8]={a,b,c,d,upscaleRaw(origin+int2(-1,-1)),upscaleRaw(origin+int2(2,-1)),
            upscaleRaw(origin+int2(-1,2)),upscaleRaw(origin+int2(2,2))};
        uint lo=(a>>shift)&255,hi=lo,lowPixel=a,highPixel=a;
        [unroll]for(uint tap=1;tap<8;++tap){
            uint v=(corners[tap]>>shift)&255;
            if(v<lo){lowPixel=corners[tap];lo=v;}
            if(v>hi){highPixel=corners[tap];hi=v;}
        }
        best=float(hi-lo);
        if(best>12){
            float3 lowColor=contourRgb(lowPixel),delta=contourRgb(highPixel)-lowColor;
            precise float dot=0,energy=1;
            [unroll]for(uint ch=0;ch<3;++ch){dot=dot+(base[ch]-lowColor[ch])*delta[ch];energy=energy+delta[ch]*delta[ch];}
            precise float t=clamp(dot/energy,0.f,1.f);
            precise float3 deviation=abs(base-(lowColor+t*delta));
            float residual=max(deviation.x,max(deviation.y,deviation.z));
            precise float reliability=clamp((best-12)*(1.f/64),0.f,1.f)*clamp((20-residual)*(1.f/12),0.f,1.f);
            precise float phase=amount*(reliability*((t*t)*(3-(t+t))-t));
            base=min(maximum,max(minimum,base+phase*delta));
        }
    }
    if(any(id.xy>=image.xy))return;
    uint3 color=uint3(base+.5f);
    packed[id.y*image.x+id.x]=0xff000000u|color.x|(color.y<<8)|(color.z<<16);
}
uint sharpLuma(uint p){return ((p&255)+2*((p>>8)&255)+((p>>16)&255)+2)>>2;}
[numthreads(16,16,1)]void finalSharpen(uint3 id:SV_DispatchThreadID){
    if(any(id.xy>=image.xy))return;
    uint x=id.x,y=id.y,w=image.x,h=image.y,at=y*w+x;
    uint own=rgba[at],up=rgba[(y?y-1:0)*w+x],down=rgba[min(y+1,h-1)*w+x],left=rgba[y*w+(x?x-1:0)],right=rgba[y*w+min(x+1,w-1)];
    if(((own&up&down&left&right)>>24)!=255){packed[at]=own;return;}
    int center=int(sharpLuma(own)),a=int(sharpLuma(up)),b=int(sharpLuma(down)),c=int(sharpLuma(left)),d=int(sharpLuma(right));
    int lo=min(center,min(min(a,b),min(c,d))),hi=max(center,max(max(a,b),max(c,d)));
    int detail=center*4-((a+b)+(c+d));
    uint gain=(image.z*384+50)/100,blend=(image.z*256+50)/100;
    int magnitude=(abs(detail)*int(gain)+512)>>10;
    int limit=(min(24,max(1,(hi-lo+4)>>2))*int(blend)+128)>>8;
    int delta=clamp(detail<0?-magnitude:magnitude,-limit,limit);
    int attenuation=max(64,256-4*max(0,2*abs(detail)-(hi-lo)-4));
    int adaptive=(abs(delta)*attenuation+128)>>8;delta=delta<0?-adaptive:adaptive;
    int r=int(own&255),g=int((own>>8)&255),blue=int((own>>16)&255);
    delta=clamp(delta,-min(r,min(g,blue)),255-max(r,max(g,blue)));
    packed[at]=(own&0xff000000u)|uint(r+delta)|(uint(g+delta)<<8)|(uint(blue+delta)<<16);
}
)HLSL";
}
