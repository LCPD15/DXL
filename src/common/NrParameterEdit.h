#pragma once
#include <cmath>
#include <cstdint>
#include <string_view>

// Append only: the index is part of the launcher/core wire protocol.
#define DXL_NR_EDIT_FIELDS(X) \
 X(nrPreset, preset, int, 0, 4) \
 X(nrStyle, style, int, 0, 2) \
 X(nrIntensity, intensity, float, 0, 1) \
 X(nrLocalTone, localTone, float, 0, 2) \
 X(nrLocalStructure, localStructure, float, 0, 2) \
 X(nrSkinStructure, skinStructure, float, 0, 1) \
 X(nrAutoMask, autoMask, bool, 0, 1) \
 X(nrUiCorrection, uiCorrection, bool, 0, 1) \
 X(nrColourStrength, colourStrength, float, 0, 1) \
 X(nrRenderScale, renderScale, float, 0.5, 1) \
 X(nrSelfLayers, selfLayers, float, 1, 3) \
 X(nrTrueLayers, trueLayers, int, 1, 5) \
 X(nrOpticalFlow, opticalFlow, bool, 0, 1) \
 X(nrOpticalFlowQuality, opticalQuality, int, 0, 2) \
 X(nrSemanticMask, semanticMask, bool, 0, 1) \
 X(nrSemBgInt, semanticBgIntensity, float, 0, 1) \
 X(nrSemanticFlipY, semanticFlipY, bool, 0, 1) \
 X(nrSemanticFeather, semanticFeather, float, 0, 8) \
 X(nrSemanticDebugView, semanticDebugView, bool, 0, 1) \
 X(nrSemOn, semanticEnabled, unsigned, 0, 4095)

namespace DXL {
struct NrEditSpec { std::string_view key; double low, high; bool integral; bool boolean = false; };
inline constexpr NrEditSpec NrEditSpecs[] = {
#define SPEC(key, member, type, low, high) {#key, low, high, std::string_view(#type) != "float", std::string_view(#type) == "bool"},
 DXL_NR_EDIT_FIELDS(SPEC)
#undef SPEC
#define REGION(n) {"nrSemInt" #n, 0, 1, false}
 REGION(0), REGION(1), REGION(2), REGION(3), REGION(4), REGION(5),
 REGION(6), REGION(7), REGION(8), REGION(9), REGION(10), REGION(11)
#undef REGION
};
inline bool EncodeNrEdit(std::string_view key, double value, uint32_t& packed) {
 if (!std::isfinite(value)) return false;
 for (unsigned i=0; i<sizeof(NrEditSpecs)/sizeof(*NrEditSpecs); ++i) {
  const auto& s=NrEditSpecs[i];
  if (s.key!=key) continue;
  if(value<s.low || value>s.high || (s.integral && std::floor(value)!=value)) return false;
  packed=(i<<24)|uint32_t(std::llround(value*1000)); return true;
 }
 return false;
}
inline bool DecodeNrEdit(uint32_t packed, unsigned& index, double& value) {
 index=packed>>24; value=(packed&0xffffff)/1000.0;
 if(index>=sizeof(NrEditSpecs)/sizeof(*NrEditSpecs)) return false;
 uint32_t checked=0; return EncodeNrEdit(NrEditSpecs[index].key,value,checked) && checked==packed;
}
}
