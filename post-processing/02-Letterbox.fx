// @dxl name_en Letterbox
// @dxl name_zh 电影黑边
// @dxl param height 0 0.3 0.1 | Bar height | 黑边高度
// @dxl param strength 0 1 1 | Strength | 强度
float3 DXL_Effect(float2 uv, float3 color) {
    float bar=step(uv.y,DXL_Parameter(0))+step(1.0-DXL_Parameter(0),uv.y);
    return color*(1.0-saturate(bar)*DXL_Parameter(1));
}
