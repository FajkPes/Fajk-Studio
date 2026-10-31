#pragma once
namespace fajk {
inline constexpr char reconstruction441Shaders[] = R"HLSL(
cbuffer Settings : register(b0) {
    uint2 size; float amount; float stronger;
    float trim; float aa; float contrastAmount; uint protectText;
    uint2 inputSize; float upscaleAmount; uint flatTile;
};
Texture2D<float4> a : register(t0);
Texture2D<float4> b : register(t1);
Texture2D<float4> directions : register(t2);
Texture2D<uint4> source : register(t3);
Texture2D<uint4> candidate : register(t4);
Texture2D<uint4> textTiles : register(t5);
Texture2D<uint4> flatMap : register(t6);
RWTexture2D<float4> output : register(u0);
RWTexture2D<uint4> packed : register(u1);
RWTexture2D<uint4> tileOutput : register(u2);
SamplerState linearClamp : register(s0);
int2 bounded(int2 p) { return clamp(p, int2(0,0), int2(size)-1); }
float3 loadA(int2 p) { return a.Load(int3(bounded(p),0)).rgb; }
float3 sampleA(float2 p) { return a.SampleLevel(linearClamp,(p+.5)/float2(size),0).rgb; }
float max3(float3 p) { return max(p.x,max(p.y,p.z)); }
[numthreads(16,8,1)] void initialize(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=size))return;
    output[id.xy]=float4(source.Load(int3(id.xy,0)));
}
float3 lowLoad(int2 p) {return float3(source.Load(int3(clamp(p,int2(0,0),int2(inputSize)-1),0)).rgb);}
[numthreads(16,8,1)] void upscale(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=size))return;int2 p=id.xy;
    if(flatTile>0){uint4 color=flatMap.Load(int3(p/int(flatTile),0));if(color.a>0){packed[p]=color;return;}}
    float2 pos=(float2(p)+.5)*float2(inputSize)/float2(size)-.5;
    int2 origin=int2(floor(pos));float2 f=pos-float2(origin);
    float3 av=lowLoad(origin),bv=lowLoad(origin+int2(1,0)),cv=lowLoad(origin+int2(0,1)),dv=lowLoad(origin+1);
    float3 base=lerp(lerp(av,bv,f.x),lerp(cv,dv,f.x),f.y),lo=min(min(av,bv),min(cv,dv)),hi=max(max(av,bv),max(cv,dv));
    float3 gx=bv-av+dv-cv,gy=cv-av+dv-bv;
    float xx=dot(gx,gx),xy=dot(gx,gy),yy=dot(gy,gy),energy=xx+yy;
    if(upscaleAmount>0&&energy>1600){
        float diff=xx-yy,disc=sqrt(diff*diff+4*xy*xy);
        float confidence=saturate((disc/max(1,energy)-.8)*5)*saturate((energy-1600)/4800);
        if(confidence>0){
            float2 n=xx>=yy?float2(diff+disc,2*xy):float2(2*xy,disc-diff);n/=sqrt(max(1,dot(n,n)));
            float stretch=1/max(.707106,max(abs(n.x),abs(n.y))),normal=1+(stretch-1)*confidence,tangent=1-.5*confidence;
            float lobe=.5-.29*confidence,clip=1/lobe;float3 sum=0;float weights=0;
            const int2 offsets[12]={int2(0,-1),int2(1,-1),int2(-1,0),int2(0,0),int2(1,0),int2(2,0),int2(-1,1),int2(0,1),int2(1,1),int2(2,1),int2(0,2),int2(1,2)};
            [unroll]for(int tap=0;tap<12;++tap){float2 o=float2(offsets[tap])-f;
                float u=dot(o,n)*normal,v=(o.y*n.x-o.x*n.y)*tangent,d=min(clip,u*u+v*v);
                float wa=lobe*d-1,wb=.4*d-1,w=wa*wa*(1.5625*wb*wb-.5625);
                sum+=lowLoad(origin+offsets[tap])*w;weights+=w;
            }
            base+=(clamp(sum/max(.0001,weights),lo,hi)-base)*(upscaleAmount*confidence);
        }
    }
    packed[p]=uint4(uint3(clamp(base+.5,0,255)),255);
}
[numthreads(16,8,1)] void horizontal(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=size))return; int2 p=id.xy;
    float3 v=0; [unroll]for(int i=-2;i<=2;++i)v+=loadA(p+int2(i,0));
    output[p]=float4(v*.2,0);
}
[numthreads(16,8,1)] void vertical(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=size))return; int2 p=id.xy;
    float3 v=0; [unroll]for(int i=-2;i<=2;++i)v+=loadA(p+int2(0,i));
    output[p]=float4(v*.2,0);
}
[numthreads(16,8,1)] void monotone(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=size))return; int2 p=id.xy; float3 own=loadA(p);
    float3 gx=b.Load(int3(bounded(p+int2(2,0)),0)).rgb-b.Load(int3(bounded(p-int2(2,0)),0)).rgb;
    float3 gy=b.Load(int3(bounded(p+int2(0,2)),0)).rgb-b.Load(int3(bounded(p-int2(0,2)),0)).rgb;
    float angle=.5*atan2(2*dot(gx,gy),dot(gx,gx)-dot(gy,gy));
    float2 normal=float2(cos(angle),sin(angle));
    float3 left=sampleA(float2(p)-8*normal), right=sampleA(float2(p)+8*normal);
    float3 delta=right-left; float energy=1+dot(delta,delta);
    if(energy<2400) { output[p]=float4(own,255);return; }
    float phase[17],variation=0;
    [unroll]for(int tap=0;tap<17;++tap) {
        float3 v=sampleA(float2(p)+(tap-8)*normal);
        phase[tap]=dot(v-left,delta)/energy;
        if(tap)variation+=abs(phase[tap]-phase[tap-1]);
    }
    if(variation>1.8) { output[p]=float4(own,255);return; }
    // Isotonic regression at the center only: the weighted max-min formula
    // equals PAVA, without its per-lane stack or divergent merge loop.
    float prefix[18];prefix[0]=0;
    [unroll]for(int pi=0;pi<17;++pi)prefix[pi+1]=prefix[pi]+saturate(phase[pi])*((pi==0||pi==16)?4:1);
    float target=0;
    [unroll]for(int start=0;start<=8;++start){float best=1;
        [unroll]for(int end=8;end<17;++end){float weight=end-start+1+(start==0?3:0)+(end==16?3:0);
            best=min(best,(prefix[end+1]-prefix[start])/weight);}
        target=max(target,best);
    }
    if(trim>0) {
        float error=max3(abs(own-(left+phase[8]*delta)));
        float reliable=saturate((25-error)/15);
        target+=reliable*(saturate((target-trim)/(1-2*trim))-target);
    }
    float3 value=own+clamp(.65*(target-phase[8])*delta,-18,18);
    output[p]=float4(clamp(value,min(own,min(left,right)),max(own,max(left,right))),255);
}
[numthreads(16,8,1)] void tensor(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=size))return; int2 p=id.xy;
    float3 gx=(loadA(p+int2(1,0))-loadA(p-int2(1,0)))*.5;
    float3 gy=(loadA(p+int2(0,1))-loadA(p-int2(0,1)))*.5;
    output[p]=float4(dot(gx,gx),dot(gx,gy),dot(gy,gy),0);
}
[numthreads(16,8,1)] void direction(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=size))return; int2 p=id.xy; float3 t=loadA(p);
    float angle=.5*atan2(2*t.y,t.x-t.z);
    float coherence=length(float2(t.x-t.z,2*t.y))/(t.x+t.z+4);
    float3 own=b.Load(int3(p,0)).rgb; float guard=0;
    const int2 axes[4]={int2(1,0),int2(0,1),int2(1,1),int2(1,-1)};
    [unroll]for(int axis=0;axis<4;++axis)[unroll]for(int d=2;d<=4;d+=2) {
        float3 da=own-b.Load(int3(bounded(p-axes[axis]*d),0)).rgb;
        float3 db=own-b.Load(int3(bounded(p+axes[axis]*d),0)).rgb;
        float correlation=dot(da,db)/sqrt(dot(da,da)*dot(db,db)+1);
        float contrast=max3(min(abs(da),abs(db)));
        guard=max(guard,saturate((correlation-.5)*2)*saturate((contrast-20)/20));
    }
    output[p]=float4(-sin(angle),cos(angle),coherence,guard);
}
[numthreads(16,8,1)] void coherent(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=size))return; int2 p=id.xy;
    float3 own=loadA(p),sum=own;float weight=1;
    float4 dir=directions.Load(int3(p,0));
    [unroll]for(int axis=0;axis<2;++axis)[unroll]for(int step=-3;step<=3;++step) {
        if(!step)continue;
        float along=step*5.0/3.0;
        float2 direction=axis?float2(-dir.y,dir.x):dir.xy;
        float distance=axis?along*(1-.85*dir.z):along;
        float3 v=sampleA(float2(p)+direction*distance);
        float error=max3(abs(v-own)),localRange=20+(!axis?stronger*36*dir.z:0);
        float r=error/localRange, w=max(0,1-r*r);
        w*=w*(step*step==9?.35:step*step==4?.7:1);
        if(stronger>0) { w*=1-dir.w*(axis?1:1-dir.z);if(axis)w*=1-stronger*dir.z; }
        sum+=w*v;weight+=w;
    }
    output[p]=float4(own+.65*(1-dir.w*(1-stronger))*(sum/weight-own),255);
}
[numthreads(16,8,1)] void finish(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=size))return;int2 p=id.xy;
    float3 own=loadA(p);float4 dir=directions.Load(int3(p,0));
    if(dir.z>=.6&&dir.w<=.5) {
        float2 offset=float2(-dir.y,dir.x)*1.5;
        float3 left=sampleA(float2(p)+offset),right=sampleA(float2(p)-offset),delta=right-left;
        float energy=1+dot(delta,delta),t=saturate(dot(own-left,delta)/energy);
        if(energy>=144)own=clamp(own+.6*(t*t*(3-2*t)-t)*delta,min(left,right),max(left,right));
    }
    uint4 original=source.Load(int3(p,0));
    float3 value=float3(original.rgb)+amount*(own-float3(original.rgb));
    packed[p]=original.a==255?uint4(uint3(clamp(value+.5,0,255)),255):original;
}
float4 raw(int2 p) {return float4(source.Load(int3(bounded(p),0)));}
float3 sampleRaw(float2 p) {
    float2 origin=floor(p),t=p-origin;int2 i=int2(origin);
    return lerp(lerp(raw(i).rgb,raw(i+int2(1,0)).rgb,t.x),
                lerp(raw(i+int2(0,1)).rgb,raw(i+1).rgb,t.x),t.y);
}
[numthreads(16,8,1)] void antialias(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=size))return;int2 p=id.xy;
    float4 own=raw(p),nw=raw(p-int2(1,1)),ne=raw(p+int2(1,-1)),sw=raw(p+int2(-1,1)),se=raw(p+1);
    float3 lo=min(own.rgb,min(min(nw.rgb,ne.rgb),min(sw.rgb,se.rgb)));
    float3 hi=max(own.rgb,max(max(nw.rgb,ne.rgb),max(sw.rgb,se.rgb))),diff=hi-lo;
    int ch=0;if(diff.y>diff.x)ch=1;if(diff.z>diff[ch])ch=2;
    if(diff[ch]<max(12,hi[ch]*.08)||min(own.a,min(min(nw.a,ne.a),min(sw.a,se.a)))<255){packed[p]=uint4(own);return;}
    float2 dir=float2(sw[ch]+se[ch]-nw[ch]-ne[ch],nw[ch]+sw[ch]-ne[ch]-se[ch]);
    float reduce=max(2,(nw[ch]+ne[ch]+sw[ch]+se[ch])*.03125);
    dir=clamp(dir/(min(abs(dir.x),abs(dir.y))+reduce),-8,8);
    float3 small=(sampleRaw(float2(p)-dir/6)+sampleRaw(float2(p)+dir/6))*.5;
    float3 wide=small*.5+(sampleRaw(float2(p)-dir*.5)+sampleRaw(float2(p)+dir*.5))*.25;
    float3 v=wide[ch]<lo[ch]||wide[ch]>hi[ch]?small:wide;
    packed[p]=uint4(uint3(clamp(own.rgb+aa*(clamp(v,lo,hi)-own.rgb)+.5,0,255)),uint(own.a));
}
[numthreads(16,8,1)] void contrast(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=size))return;int2 p=id.xy;float4 own=raw(p),taps[4];
    taps[0]=raw(p-int2(2,0));taps[1]=raw(p+int2(2,0));taps[2]=raw(p-int2(0,2));taps[3]=raw(p+int2(0,2));
    float3 left=own.rgb,right=own.rgb;float best=0;
    [unroll]for(int ch=0;ch<3;++ch){float lo=own[ch],hi=own[ch];float3 lp=own.rgb,hp=own.rgb;
        [unroll]for(int k=0;k<4;++k){if(taps[k][ch]<lo){lo=taps[k][ch];lp=taps[k].rgb;}if(taps[k][ch]>hi){hi=taps[k][ch];hp=taps[k].rgb;}}
        if(hi-lo>best){best=hi-lo;left=lp;right=hp;}
    }
    if(best<20||min(own.a,min(min(taps[0].a,taps[1].a),min(taps[2].a,taps[3].a)))<255){packed[p]=uint4(own);return;}
    float3 delta=right-left;float t=saturate(dot(own.rgb-left,delta)/(1+dot(delta,delta)));
    float residual=max3(abs(own.rgb-(left+t*delta))),compatible=saturate((20-residual)*.1);
    float shift=contrastAmount*compatible*t*(1-t)*(2*t-1);
    float3 v=clamp(own.rgb+shift*delta,min(own.rgb,min(left,right)),max(own.rgb,max(left,right)));
    packed[p]=uint4(uint3(clamp(v+.5,0,255)),uint(own.a));
}
float luminance(float3 rgb){return (rgb.r+rgb.b+2*rgb.g)*.25;}
groupshared uint hits,neutrals,minX,maxX,minY,maxY;
[numthreads(16,16,1)] void textMask(uint3 id:SV_DispatchThreadID,uint3 group:SV_GroupID,uint index:SV_GroupIndex){
    if(!index){hits=0;neutrals=0;minX=minY=65535;maxX=maxY=0;}
    GroupMemoryBarrierWithGroupSync();
    if(all(id.xy<size)){
        int2 p=id.xy;float4 own=raw(p);float hi=max3(own.rgb),lo=min(own.r,min(own.g,own.b));
        if(hi-lo<=32||hi<48)InterlockedAdd(neutrals,1);
        if(own.a==255&&hi-lo<=48&&hi>=52){
            float center=luminance(own.rgb),variation=0;
            variation=max(variation,abs(center-luminance(raw(p+int2(1,0)).rgb)));
            variation=max(variation,abs(center-luminance(raw(p-int2(1,0)).rgb)));
            variation=max(variation,abs(center-luminance(raw(p+int2(0,1)).rgb)));
            variation=max(variation,abs(center-luminance(raw(p-int2(0,1)).rgb)));
            bool stroke=false;
            if(variation>=6){
                [unroll]for(int axis=0;axis<2;++axis)[unroll]for(int d=2;d<=4;d+=2){
                    int2 offset=axis?int2(0,d):int2(d,0);float4 left=raw(p+offset),right=raw(p-offset);
                    float l=luminance(left.rgb),r=luminance(right.rgb),da=center-l,db=center-r;
                    stroke=stroke||(da*db>0&&min(abs(da),abs(db))>=16&&abs(l-r)<=24&&left.a==255&&right.a==255);
                }
            }
            if(stroke){InterlockedAdd(hits,1);InterlockedMin(minX,id.x);InterlockedMax(maxX,id.x);InterlockedMin(minY,id.y);InterlockedMax(maxY,id.y);}
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if(!index){bool keep=hits>=4&&((maxX-minX>=1&&maxY-minY>=3)||(maxX-minX>=3&&maxY-minY>=1)||maxX-minX>=8||maxY-minY>=8);
        uint2 extent=min(uint2(16,16),size-group.xy*16);tileOutput[group.xy]=uint4(neutrals,keep,extent.x*extent.y,0);}
}
bool keepTile(int2 p){
    int2 count=int2((size+15)/16);p=clamp(p,int2(0,0),count-1);
    if(textTiles.Load(int3(p,0)).y==0)return false;uint neutral=0,total=0;
    [loop]for(int y=max(0,p.y-1);y<=min(count.y-1,p.y+1);++y)
        [loop]for(int x=max(0,p.x-1);x<=min(count.x-1,p.x+1);++x){uint4 t=textTiles.Load(int3(x,y,0));neutral+=t.x;total+=t.z;}
    return neutral*100>=total*80;
}
[numthreads(16,8,1)] void textFinish(uint3 id:SV_DispatchThreadID){
    if(any(id.xy>=size))return;int2 p=id.xy;
    bool keep=false;
    [loop]for(int y=max(0,p.y-2)/16;y<=min(int(size.y)-1,p.y+2)/16;++y)
        [loop]for(int x=max(0,p.x-2)/16;x<=min(int(size.x)-1,p.x+2)/16;++x)keep=keep||keepTile(int2(x,y));
    packed[p]=keep?source.Load(int3(p,0)):candidate.Load(int3(p,0));
}
)HLSL";
}

