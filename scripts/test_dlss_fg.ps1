param([string]$OutDir = 'dlss-fg-smoke-1.0')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$OutDir = Resolve-DxlOutput $OutDir
Set-Location $SourceRoot
& ./scripts/build_core.ps1 -OutDir $OutDir -LibraryOnly
New-Item -ItemType Directory -Path "$OutDir/ngx","$OutDir/obj/fg-test" -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $DependencyRoot 'dlss-fg-310.9.16/nvngx_dlssg.dll') -Destination "$OutDir/ngx/nvngx_dlssg.dll"
Copy-Item -LiteralPath (Join-Path $DependencyRoot 'runtime/nvngx_dlssnr.dll') -Destination "$OutDir/ngx/nvngx_dlssnr.dll"
$sr = Join-Path $DependencyRoot 'dlss/lib/Windows_x86_64/rel/nvngx_dlss.dll'
if (Test-Path -LiteralPath $sr) { Copy-Item -LiteralPath $sr -Destination "$OutDir/ngx/nvngx_dlss.dll" }
$flags=@('/nologo','/O2','/MT','/std:c++20','/utf-8','/EHsc','/DUNICODE','/D_UNICODE','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/Isrc/core','/Ithird_party/fidelityfx/sdk/include','/Ithird_party/fidelityfx/sdk/src/backends/shared',('/I'+(Join-Path $DependencyRoot 'dlss/include')),"/Fo$OutDir/obj/fg-test/")
$sources=@('tests/fg_smoke.cpp','src/core/DlssFgBackend.cpp','src/core/DlssNrFilter.cpp','src/core/ColorGrading.cpp','src/core/CommandListTracker.cpp','src/core/FreezeWatchdog.cpp','src/core/NgxSession.cpp','src/core/OpticalFlow.cpp','src/core/OpticalFlowShaders.cpp','src/core/ComputePasses.cpp')
& cl.exe @flags @sources /link "/OUT:$OutDir/fg_smoke.exe" "$OutDir/ffx_optical.lib" (Join-Path $DependencyRoot 'dlss/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib') d3d12.lib dxgi.lib dxguid.lib shell32.lib ole32.lib advapi32.lib user32.lib d3dcompiler.lib windowscodecs.lib psapi.lib
if ($LASTEXITCODE) {throw 'FG smoke build failed'}
foreach($mode in @('--rgba8','--bgra','--rgb10')) {
    & "$OutDir/fg_smoke.exe" $mode
    if ($LASTEXITCODE) {throw "DLSS FG GPU smoke $mode failed"}
}
