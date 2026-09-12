param([string]$OutDir = '', [ValidateSet('Full','Lean')][string]$Runtime = 'Lean', [switch]$Test, [switch]$ValidateOnly)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$OutDir = Resolve-DxlOutput $OutDir
$runtimeFolder = if ($Runtime -eq 'Lean') { 'runtime/tensorrt-lean' } else { 'runtime/tensorrt' }
$runtimeDll = if ($Runtime -eq 'Lean') { 'nvinfer_lean_11.dll' } else { 'nvinfer_11.dll' }
$modelRoot = Join-Path $DependencyRoot 'models-lean'
$required = @(
    (Join-Path $DependencyRoot ($runtimeFolder+'/'+$runtimeDll)),
    (Join-Path $modelRoot 'yolo11n-seg.plan'),
    (Join-Path $DependencyRoot 'runtime/nvngx_dlssnr.dll'),
    (Join-Path $DependencyRoot 'dlss/include/nvsdk_ngx.h'),
    (Join-Path $DependencyRoot 'webview2/include/WebView2.h'))
foreach ($file in $required) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf) -or (Get-Item -LiteralPath $file).Length -eq 0) { throw "Dependency missing: $file. See DEPENDENCIES.md." }
}
if ($ValidateOnly) { Write-Output 'PASS external dependency preflight'; return }
& (Join-Path $PSScriptRoot 'build_launcher.ps1') -OutDir $OutDir -Test:$Test
& (Join-Path $PSScriptRoot 'build_core.ps1') -OutDir $OutDir
function Stage([string]$Source,[string]$Relative) {
    if (!(Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Missing asset: $Source" }
    $destination = Join-Path $OutDir $Relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $destination -Force
}
foreach ($name in @('nvngx_dlssnr.dll','nvngx_dlss.dll','nvngx_dlssg.dll')) { Stage (Join-Path $DependencyRoot ('runtime/'+$name)) ('ngx/'+$name) }
# Single YOLO model is integrated into every complete package.
Stage (Join-Path $DependencyRoot ($runtimeFolder+'/'+$runtimeDll)) ('extensions/semantic/'+$runtimeDll)
Stage (Join-Path $modelRoot 'yolo11n-seg.plan') 'extensions/semantic/models/yolo11n-seg.plan'
Stage (Join-Path $DependencyRoot ($runtimeFolder+'/LICENSE.txt')) 'licenses/TensorRT-Runtime-LICENSE.txt'
foreach ($entry in @(
    @('LICENSE','LICENSE'),
    @('SOURCE_CODE.md','SOURCE_CODE.md'),
    @('docs/UPDATES.md','UPDATES.md'),
    @('third_party/imgui/LICENSE.txt','licenses/ImGui-LICENSE.txt'),
    @('third_party/minhook/LICENSE.txt','licenses/MinHook-LICENSE.txt'),
    @('third_party/tensorrt/LICENSE.txt','licenses/TensorRT-Headers-LICENSE.txt'),
    @('NRFG_OPTICAL_FLOW_NOTICES.txt','licenses/FidelityFX-NOTICES.txt'),
    @('third_party/pix/license.txt','licenses/PIX-LICENSE.txt'),
    @('third_party/pix/ThirdPartyNotices.txt','licenses/PIX-NOTICES.txt'),
    @('licenses/AGPL-3.0.txt','licenses/YOLO-AGPL-3.0.txt'),
    @('licenses/GPL-3.0.txt','licenses/GPL-3.0.txt'),
    @('licenses/NR-Color-Conversion-NOTICE.txt','licenses/NR-Color-Conversion-NOTICE.txt'),
    @('THIRD_PARTY_NOTICES.md','THIRD_PARTY_NOTICES.md'),
    @('LICENSE_STATUS.md','LICENSE_STATUS.md'),
    @('CHANGELOG.md','CHANGELOG.md'),
    @('docs/DXL-Guide-ZH.docx','DXL-Guide-ZH.docx'),
    @('docs/DXL-Guide-EN.docx','DXL-Guide-EN.docx'),
    @('docs/README_ZH.md','README_ZH.md'),
    @('docs/README_EN.md','README_EN.md'))) { Stage (Join-Path $SourceRoot $entry[0]) $entry[1] }
Stage (Join-Path $DependencyRoot 'dlss/LICENSE.txt') 'licenses/NVIDIA-RTX-SDK-LICENSE.txt'
Stage (Join-Path $DependencyRoot 'webview2/licenses/LICENSE.txt') 'licenses/WebView2-LICENSE.txt'
Stage (Join-Path $DependencyRoot 'webview2/licenses/NOTICE.txt') 'licenses/WebView2-NOTICE.txt'
Write-Output "PASS DXL 0.5 complete build: $OutDir"
