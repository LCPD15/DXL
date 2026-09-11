# Dependencies and build inputs

All downloaded SDKs, runtime files, weights and generated plans belong outside
source, under DXL_WORKSPACE/dependencies/. Never commit them.

| Component | Source / download | Location | License / packaging status |
|---|---|---|---|
| ImGui | https://github.com/ocornut/imgui | third_party/imgui | MIT; source and notice included |
| MinHook | https://github.com/TsudaKageyu/minhook | third_party/minhook | BSD; source and notices included |
| FidelityFX / D3DX12 | https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK | third_party/fidelityfx | MIT; used source and notices included |
| PIX headers | https://www.nuget.org/packages/WinPixEventRuntime | third_party/pix | Included headers and their notices |
| TensorRT headers | https://github.com/NVIDIA/TensorRT | third_party/tensorrt | Apache-2.0 source; full upstream license included |
| DLSS SDK | https://github.com/NVIDIA/DLSS/tree/a291cc7d2cc642a51566f3dfd5376f635cd1b284 | dependencies/dlss | NVIDIA RTX SDK license; not in source export |
| WebView2 SDK | https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.3485.44 | dependencies/webview2 | Microsoft license; fetched externally |
| TensorRT Lean 11.2.1.2 | https://pypi.org/project/tensorrt-lean-cu13-libs/11.2.1.2/ | dependencies/runtime/tensorrt-lean | Proprietary runtime; version-specific redistribution conditions |
| YOLO11n-seg weights | https://github.com/ultralytics/assets/releases/download/v8.3.0/yolo11n-seg.pt | external model work directory | AGPL-3.0 or applicable Enterprise license |
| YOLO export tools | https://github.com/ultralytics/ultralytics | external Python environment | AGPL-3.0; https://www.ultralytics.com/license |
| NR runtime | https://github.com/SAOG0721/Magpie/releases | dependencies/runtime/nvngx_dlssnr.dll | Community source reference only, NOT an NVIDIA authorization; no authorized download for the exact modified file has been verified |

## Build DXL 0.1

Install Visual Studio 2022 Build Tools with the C++ x64 tools and a Windows SDK.
Use PowerShell from the source root. Keep DXL_WORKSPACE outside the checkout:

```powershell
$env:DXL_WORKSPACE = 'D:\DXL-Workspace'
./scripts/fetch-deps.ps1
# Prepare the runtime and model inputs described below, then:
./scripts/build_dxl.ps1 -Runtime Lean -Test
./scripts/release_dxl.ps1 -SourceDir "$env:DXL_WORKSPACE/build/dxl-0.1" -OutDir 'D:\DXL-release/DXL-v0.1'
```

The source includes FG compatibility and runtime route switching. No private
DXL implementation library is required. Third-party SDKs and runtimes keep their
own terms. See SOURCE_CODE.md and LICENSE for DXL's source and license.

### Prepare external inputs

scripts/fetch-deps.ps1 fetches the pinned DLSS and WebView2 SDKs into the external
workspace. It does not download or patch an NR runtime.

Place authorized runtime inputs in dependencies/runtime/:
nvngx_dlss.dll, nvngx_dlssg.dll, nvngx_dlssnr.dll.
Extract nvinfer_lean_11.dll and its original LICENSE.txt from the official Lean
wheel to dependencies/runtime/tensorrt-lean/.

Export a YOLO11n-seg model to ONNX using Ultralytics, then build a TensorRT engine
with the matching TensorRT SDK for the target hardware. Copy the validated
yolo11n-seg.plan to dependencies/models-lean/. Example model export:

```python
from ultralytics import YOLO
YOLO("yolo11n-seg.pt").export(format="onnx", imgsz=640, dynamic=False, opset=17)
```

The exact ONNX input (including weights) used for the bundled model is available
in [DXL-v0.1-model-source.zip](https://github.com/LCPD15/DXL/releases/download/v0.1/DXL-v0.1-model-source.zip).
Its SHA256 is `0bc32bc92e985b881141ef9bd2216e2a746f70519d0d24da9fc85decc4428cf4`.
The bundled engine uses the original TensorRT 11.2.1.2 builder defaults and a
2 GiB workspace, without VERSION_COMPATIBLE or EXCLUDE_LEAN_RUNTIME flags:

```powershell
python scripts/build_release_model.py --onnx '<external-path>/yolo11n-seg.onnx' --out '<external-output>/yolo11n-seg.plan'
```

`scripts/build_lean_model.py` is an alternative builder that enables
VERSION_COMPATIBLE and EXCLUDE_LEAN_RUNTIME; it was not used for the bundled plan.
Both recipes need the builder SDK; the app uses the smaller Lean runtime.
TensorRT tactic selection can produce different engine bytes when rebuilt.

The current parser expects the YOLO11n-seg detection/prototype outputs at 640×640.
A .plan is not automatically portable to another TensorRT release or GPU.
Keep model conversion/build tools and outputs in an external workspace.
See NVIDIA's engine portability guidance:
https://docs.nvidia.com/deeplearning/tensorrt/latest/inference-library/advanced.html

## Optional Vulkan fixture

`scripts/test_vulkan_dlss.ps1` also needs [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers)
and [volk](https://github.com/zeux/volk), checked out under `dependencies/Vulkan-Headers`
and `dependencies/volk`. The fixture uses the DLSS Vulkan headers and SR library
from the DLSS SDK listed above. These test dependencies are not part of the player package.

## Generated shader headers

The checked-in shader headers are generated implementation assets, not downloaded
runtime DLLs. Corresponding HLSL is included. Compute shaders can be regenerated
with scripts/build-shaders.cmd and the Windows SDK. AMD optical flow shader source
and the generated headers retain the AMD notice. Regenerate them with
scripts/build_optical_shaders.ps1; temporary compiler output stays outside source.
