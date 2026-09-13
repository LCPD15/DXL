// Spatial multi-resolution bloom. There is no exposure history or adaptation.
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Target : register(u0);
SamplerState LinearClamp : register(s0);
cbuffer BloomParams : register(b0) {
    uint SourceWidth, SourceHeight, TargetWidth, TargetHeight;
    uint FirstLevel, InputLinear;
    float Threshold, SoftKnee;
    float Radius, Unused0, Unused1, Unused2;
};
float3 DisplayEncode(float3 x) {
    float3 a=abs(x);
    return sign(x)*lerp(1.055*pow(max(a,1e-10),1.0/2.4)-0.055,12.92*a,step(a,0.0031308));
}
float3 ReadGlow(float2 position) {
    uint actualWidth,actualHeight;
    Source.GetDimensions(actualWidth,actualHeight);
    float2 uv=clamp(position+0.5,0.5,float2(SourceWidth,SourceHeight)-0.5)/float2(actualWidth,actualHeight);
    float3 color=Source.SampleLevel(LinearClamp,uv,0).rgb;
    if(FirstLevel) {
        if(InputLinear) color=DisplayEncode(color);
        // Bound only the auxiliary glow, not the source image or its HDR range.
        color=all((asuint(color)&0x7f800000)!=0x7f800000)?clamp(color,0,16):0;
        float luma=dot(color,float3(0.2126,0.7152,0.0722));
        float knee=max(Threshold*SoftKnee,0.00001);
        float soft=clamp(luma-Threshold+knee,0,2*knee);
        soft=soft*soft/(4*knee);
        float contribution=max(luma-Threshold,soft)/max(luma,0.00001);
        color*=saturate(contribution);
    }
    return color;
}
[numthreads(8,8,1)]
void BloomDownsample(uint3 id:SV_DispatchThreadID) {
    if(id.x>=TargetWidth||id.y>=TargetHeight)return;
    float2 center=(float2(id.xy)+0.5)*float2(SourceWidth,SourceHeight)/float2(TargetWidth,TargetHeight)-0.5;
    float3 glow=0;
    [unroll] for(int y=-1;y<=1;y++) [unroll] for(int x=-1;x<=1;x++) {
        float weight=(x==0?2:1)*(y==0?2:1);
        glow+=ReadGlow(center+float2(x,y)*Radius)*weight;
    }
    Target[id.xy]=float4(glow/16,1);
}
