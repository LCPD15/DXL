param([string]$OutDir = '', [switch]$LibraryOnly)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$OutDir = Resolve-DxlOutput $OutDir
Set-Location (Split-Path $PSScriptRoot -Parent)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC x64 tools not found' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars) | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -LiteralPath ('Env:'+$matches[1]) -Value $matches[2] }
}
$compiler = Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64/cl.exe'
New-Item -ItemType Directory -Path "$OutDir/obj/core","$OutDir/obj/ffx" -Force | Out-Null
$sdk = 'third_party/fidelityfx/sdk'
$ffxLibrary = Get-Item -LiteralPath "$OutDir/ffx_optical.lib" -ErrorAction SilentlyContinue
$ffxInputs = @(Get-ChildItem -LiteralPath third_party/fidelityfx -Recurse -File)
if (!$ffxLibrary -or @($ffxInputs | Where-Object { $_.LastWriteTimeUtc -gt $ffxLibrary.LastWriteTimeUtc }).Count) {
    $ffxSources = @("$sdk/src/backends/dx12/ffx_dx12.cpp", "$sdk/src/components/opticalflow/ffx_opticalflow.cpp", "$sdk/src/shared/ffx_assert.cpp", "$sdk/src/shared/ffx_object_management.cpp", "$sdk/src/shared/ffx_breadcrumbs_list.cpp")
    & $compiler /nologo /c /O2 /MT /EHsc /std:c++20 /utf-8 /DNDEBUG /DNOMINMAX /DUNICODE /D_UNICODE /DFFX_OF "/I$sdk/include" "/I$sdk/src/shared" "/I$sdk/src/backends/shared" "/Fo$OutDir/obj/ffx/" @ffxSources
    if ($LASTEXITCODE) { throw 'FidelityFX compile failed' }
    $objects = Get-ChildItem -LiteralPath "$OutDir/obj/ffx" -Filter '*.obj' | ForEach-Object FullName
    & lib.exe /nologo "/OUT:$OutDir/ffx_optical.lib" @objects
    if ($LASTEXITCODE) { throw 'FidelityFX link failed' }
}
if ($LibraryOnly) { return }
$flags = @('/nologo','/O2','/Oi','/MT','/EHsc','/GR','/std:c++20','/utf-8','/W3',
    '/D_CRT_SECURE_NO_WARNINGS','/DUNICODE','/D_UNICODE','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN',
    '/Isrc/core','/Ithird_party/imgui','/Ithird_party/imgui/backends','/Ithird_party/minhook/include',
    ('/I'+(Join-Path $DependencyRoot 'dlss/include')),'/Ithird_party/tensorrt/include','/Ithird_party/cuda-stub',
    '/Ithird_party/fidelityfx/sdk/include','/Ithird_party/fidelityfx/sdk/src/backends/shared',"/Fo$OutDir/obj/core/")
$core = @('core','NgxSession','DlssSrUpscaler','SwapChainScaler','DlssNrFilter','DlssNrFilter11','SegMaskFilter',
    'ComputePasses','ColorGrading','ReShadeBridge','CommandListTracker','DepthTracker','FreezeWatchdog','NgxCallerProbe','NgxEavesdrop',
    'ChainInject','LegacyGraphics','ReUiBackend','OpticalFlow','OpticalFlowShaders') | ForEach-Object { "src/core/$_.cpp" }
$deps = @('third_party/imgui/imgui.cpp','third_party/imgui/imgui_draw.cpp','third_party/imgui/imgui_tables.cpp',
    'third_party/imgui/imgui_widgets.cpp','third_party/imgui/backends/imgui_impl_dx12.cpp','third_party/imgui/backends/imgui_impl_dx11.cpp',
    'third_party/minhook/src/hook.c','third_party/minhook/src/buffer.c','third_party/minhook/src/trampoline.c','third_party/minhook/src/hde/hde64.c')
& $compiler @flags @core @deps /link /DLL "/OUT:$OutDir/DXL-core.dll" "/MAP:$OutDir/DXL-core.map" "/IMPLIB:$OutDir/DXL-core.lib" `
    d3d11.lib d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib psapi.lib user32.lib shell32.lib ole32.lib windowscodecs.lib advapi32.lib gdi32.lib `
    (Join-Path $DependencyRoot 'dlss/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib') "$OutDir/ffx_optical.lib"
if ($LASTEXITCODE) { throw "DXL core compilation failed: $LASTEXITCODE" }
