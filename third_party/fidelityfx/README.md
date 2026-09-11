# FidelityFX Optical Flow subset

Upstream: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4

Pinned SDK: **v1.1.4**, Optical Flow component **1.1.2**. The host/backend and
shader sources are an upstream subset. AMD's MIT notice is in `LICENSE.txt` and
in the source headers. The included D3DX12 helper is Microsoft MIT code.
`NRFG_OPTICAL_FLOW_NOTICES.txt` at the project root accompanies binary releases.

Local integration:

- Disabled `ENABLE_PIX_CAPTURES` in `sdk/src/backends/dx12/ffx_dx12.cpp`; no PIX
  runtime dependency is shipped.
- Build only Optical Flow and the DX12 backend via `build/build_fidelityfx.ps1`.
- Replace the SDK-wide shader accessor with `core/OpticalFlowShaders.cpp`.
  Only the seven FP32, default-wave-size, SDR-input kernels are embedded.
  NRFG prepares its display/encoded colour input before calling the SDK.
- `build/build_optical_shaders.ps1` generates DXIL and reflection bindings with
  Windows SDK DXC. Generated headers are committed; users do not need DXC.
- Motion resampling, gating, UI and lifetime management are NRFG code, not a
  copied Magpie integration. No GPL Magpie implementation was incorporated.

The standalone reproduction is `build/build_optical_test.ps1`, followed by
`build/optical_flow.exe --4k --quality=1`. The default run enables D3D12 validation;
use `--no-debug` only for performance measurement.
