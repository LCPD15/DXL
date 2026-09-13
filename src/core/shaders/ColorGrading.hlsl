Texture2D<float4> Source : register(t0);
Texture3D<float4> Lut : register(t1);
Texture2D<float4> Bloom0 : register(t2);
Texture2D<float4> Bloom1 : register(t3);
Texture2D<float4> Bloom2 : register(t4);
Texture2D<float4> Bloom3 : register(t5);
Texture2D<float4> Bloom4 : register(t6);
RWTexture2D<float4> Target : register(u0);
SamplerState LinearClamp : register(s0);

cbuffer Params : register(b0) {
    uint Width, Height, Flags, Frame;
    float Exposure, Contrast, Saturation, Temperature;
    float Tint, Shadows, Midtones, Highlights;
    float Sharpen, Bloom, Vignette, Grain;
    float Monochrome, LutIntensity, LutSize, Padding;
    float BloomScatter, BloomSaturation, BloomUnused0, BloomUnused1;
};

// Signed, extended sRGB conversion preserves negative and above-white linear
// HDR values. It is not a display tonemapper and never clamps the source.
float3 DisplayEncode(float3 x) {
    float3 a = abs(x);
    float3 v = lerp(1.055 * pow(max(a, 1e-10), 1.0 / 2.4) - 0.055,
        12.92 * a, step(a, 0.0031308));
    return sign(x) * v;
}
float3 DisplayDecode(float3 x) {
    float3 a = abs(x);
    float3 v = lerp(pow((a + 0.055) / 1.055, 2.4), a / 12.92, step(a, 0.04045));
    return sign(x) * v;
}
float3 WorkColor(float3 c) { return (Flags & 0x10000) ? DisplayEncode(c) : c; }
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 ReadAt(int2 p) {
    return WorkColor(Source.Load(int3(clamp(p, int2(0,0), int2(Width-1,Height-1)),0)).rgb);
}
uint Hash(uint x) {
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; return x ^ (x >> 16);
}
float3 SampleLut(float3 color) {
    float3 position=color*(LutSize-1);
    int3 cell=min(int3(position),int(LutSize)-2);
    float3 f=position-cell;
    // Explicit interpolation avoids low-precision fixed-function fractions
    // making an identity LUT alter flat gradients, especially after HDR decode.
    float3 z0=lerp(lerp(Lut.Load(int4(cell,0)).rgb,Lut.Load(int4(cell+int3(1,0,0),0)).rgb,f.x),
        lerp(Lut.Load(int4(cell+int3(0,1,0),0)).rgb,Lut.Load(int4(cell+int3(1,1,0),0)).rgb,f.x),f.y);
    float3 z1=lerp(lerp(Lut.Load(int4(cell+int3(0,0,1),0)).rgb,Lut.Load(int4(cell+int3(1,0,1),0)).rgb,f.x),
        lerp(Lut.Load(int4(cell+int3(0,1,1),0)).rgb,Lut.Load(int4(cell+int3(1,1,1),0)).rgb,f.x),f.y);
    return lerp(z0,z1,f.z);
}

[numthreads(8,8,1)]
void GradeMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Width || id.y >= Height) return;
    int2 p = int2(id.xy);
    float4 original = Source.Load(int3(p,0));
    float3 c = WorkColor(original.rgb);

    [branch] if (Flags & (1u << 8)) {
        float3 a=ReadAt(p+int2(1,0)), b=ReadAt(p-int2(1,0));
        float3 d=ReadAt(p+int2(0,1)), e=ReadAt(p-int2(0,1));
        float3 lo=min(c,min(min(a,b),min(d,e)));
        float3 hi=max(c,max(max(a,b),max(d,e)));
        // A small bounded overshoot makes existing edge extrema sharpen too;
        // clamping exactly to local extrema erased the old effect on edges.
        float3 allowance=min((hi-lo)*0.18,0.08)*(1+0.5*max(Sharpen-1,0));
        c=clamp(c+(c-(a+b+d+e)*0.25)*(Sharpen*2.0),lo-allowance,hi+allowance);
    }
    [branch] if (Flags & (1u << 9)) {
        float2 uv=(float2(id.xy)+0.5)/float2(Width,Height);
        float scatter=BloomScatter;
        float w1=scatter, w2=w1*scatter, w3=w2*scatter, w4=w3*scatter;
        float3 glow=Bloom0.SampleLevel(LinearClamp,uv,0).rgb;
        [branch] if(scatter>0.00001) {
            glow+=Bloom1.SampleLevel(LinearClamp,uv,0).rgb*w1;
            glow+=Bloom2.SampleLevel(LinearClamp,uv,0).rgb*w2;
            glow+=Bloom3.SampleLevel(LinearClamp,uv,0).rgb*w3;
            glow+=Bloom4.SampleLevel(LinearClamp,uv,0).rgb*w4;
        }
        glow/=1+w1+w2+w3+w4;
        glow=max(lerp(Luma(glow).xxx,glow,BloomSaturation),0);
        c+=glow*Bloom;
    }
    [branch] if (Flags & (1u << 0)) {
        // Exposure is measured in linear-light stops on either input route.
        c=DisplayEncode(DisplayDecode(c)*exp2(Exposure));
    }
    [branch] if (Flags & (1u << 1)) c=(c-0.5)*Contrast+0.5;
    [branch] if (Flags & (1u << 2)) c=lerp(Luma(c).xxx,c,Saturation);
    [branch] if (Flags & (1u << 3)) c*=exp2(float3(0.3,0,-0.3)*Temperature);
    [branch] if (Flags & (1u << 4)) c*=exp2(float3(0.15,-0.3,0.15)*Tint);
    [branch] if (Flags & ((1u << 5)|(1u << 6)|(1u << 7))) {
        float l=saturate(Luma(c));
        float s=1-smoothstep(0.0,0.55,l);
        float h=smoothstep(0.45,1.0,l);
        float m=max(0,1-s-h);
        float stops=0;
        [branch] if (Flags & (1u << 5)) stops+=Shadows*s;
        [branch] if (Flags & (1u << 6)) stops+=Midtones*m;
        [branch] if (Flags & (1u << 7)) stops+=Highlights*h;
        c=DisplayEncode(DisplayDecode(c)*exp2(stops*2.0));
    }
    [branch] if (Flags & (1u << 13)) {
        float3 bounded=saturate(c);
        float3 mapped=SampleLut(bounded);
        // A display LUT defines 0..1 only. Apply its delta and retain the HDR
        // residual instead of clipping every bright pixel to LUT white.
        c+=(mapped-bounded)*LutIntensity;
    }
    [branch] if (Flags & (1u << 12)) c=lerp(c,Luma(c).xxx,Monochrome);
    [branch] if (Flags & (1u << 10)) {
        float2 uv=(float2(id.xy)+0.5)/float2(Width,Height)*2-1;
        c*=1-Vignette*0.85*smoothstep(0.15,1.4,length(uv));
    }
    [branch] if (Flags & (1u << 11)) {
        float noise=(Hash(id.x+id.y*Width+Hash(Frame)) & 0xffff)/65535.0-0.5;
        // Full strength is deliberately visible; the slider retains a fine
        // low-strength range. This display pass is not temporally accumulated.
        c+=noise*(Grain*0.24);
    }
    if (Flags & 0x10000) c=DisplayDecode(c);
    // Very strong glow on already-bright HDR content must not overflow a
    // half-float output into infinity. This is a storage bound, not tonemapping.
    if(Flags & (1u << 9)) {
        c=all((asuint(c)&0x7f800000)!=0x7f800000)?c:original.rgb;
        if(Padding>0)c=clamp(c,-Padding,Padding);
    }
    // Original alpha may carry composition data; none of these effects owns it.
    Target[id.xy]=float4(c,original.a);
}
