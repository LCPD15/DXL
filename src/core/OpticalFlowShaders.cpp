// Minimal shader accessor for the MIT FidelityFX Optical Flow component.
// All permutations use the same portable FP32 shader. We deliberately do not
// advertise a wave64/FP16 specialization; there are no other SDK effects here.
#include <FidelityFX/host/ffx_opticalflow.h>
#include <ffx_shader_blobs.h>
#include <cstring>
#include "shaders/precompiled/optical/Blobs.h"
extern "C" FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effect, FfxPass pass,
    FfxBindStage, uint32_t, FfxShaderBlob* output) {
    if (!output || effect != FFX_EFFECT_OPTICALFLOW || pass >= FFX_OPTICALFLOW_PASS_COUNT)
        return FFX_ERROR_INVALID_ARGUMENT;
    memcpy(output, &kOpticalBlobs[pass], sizeof(*output));
    return FFX_OK;
}
extern "C" FfxErrorCode ffxIsWave64(FfxEffect, uint32_t, bool& wave64) {
    wave64 = false;
    return FFX_OK;
}
