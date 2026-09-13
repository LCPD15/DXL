// Test-only ReShade source. This file is not copied into the release package.
#include "ReShade.fxh"
uniform float Strength < ui_type = "slider"; ui_min = -2.0; ui_max = 2.0; ui_label = "Strength"; ui_label_zh = "强度"; ui_tooltip = "Runtime metadata"; > = 0.25;
uniform float3 Tint < ui_type = "color"; > = float3(0.1, 0.2, 0.3);
uniform int Mode < ui_type = "combo"; ui_items = "One\0Two\0Three\0"; > = 1;
uniform bool Enabled = true;
uniform uint Counter < ui_type = "slider"; ui_min = 0; ui_max = 4; > = 2;
uniform float2 Weights[2] = { float2(0.2, 0.3), float2(0.4, 0.5) };
uniform float Locked < noedit = true; > = 0.6;
uniform float Clock < source = "timer"; >;
uniform float Hidden < hidden = true; > = 0.7;
float4 Validate(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target {
    float3 color = tex2D(ReShade::BackBuffer, uv).rgb;
    return float4(color + (Strength + Tint + Mode + Enabled + Counter + Weights[0].x + Weights[1].y + Locked + Clock + Hidden) * 0.000001, 1);
}
technique UiPrimary < ui_label = "Metadata primary"; > { pass { VertexShader = PostProcessVS; PixelShader = Validate; } }
technique UiSecondary { pass { VertexShader = PostProcessVS; PixelShader = Validate; } }
