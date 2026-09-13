// @dxl name_en Sepia
// @dxl name_zh 复古棕褐
// @dxl param strength 0 1 0.5 | Strength | 强度
float3 DXL_Effect(float2 uv, float3 color) {
    float3 sepia=float3(dot(color,float3(0.393,0.769,0.189)),
                       dot(color,float3(0.349,0.686,0.168)),
                       dot(color,float3(0.272,0.534,0.131)));
    return lerp(color,sepia,DXL_Parameter(0));
}
