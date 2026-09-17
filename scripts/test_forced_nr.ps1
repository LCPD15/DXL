param([string]$OutDir='forced-nr-tests')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$OutDir=Resolve-DxlOutput $OutDir
Set-Location $SourceRoot
& ./scripts/build_core.ps1 -OutDir $OutDir -LibraryOnly
New-Item -ItemType Directory -Force -Path "$OutDir/obj/tests","$OutDir/ngx" | Out-Null
Copy-Item -LiteralPath (Join-Path $DependencyRoot 'runtime/nvngx_dlssnr.dll') -Destination "$OutDir/ngx/nvngx_dlssnr.dll" -Force
$flags = @('/nologo','/O2','/MT','/EHsc','/std:c++20','/utf-8','/W3','/D_CRT_SECURE_NO_WARNINGS',
    '/DUNICODE','/D_UNICODE','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/Isrc/core','/Isrc/common','/Ithird_party/minhook/include',
    ('/I'+(Join-Path $DependencyRoot 'dlss/include')),'/Ithird_party/fidelityfx/sdk/include','/Ithird_party/fidelityfx/sdk/src/backends/shared',"/Fo$OutDir/obj/tests/")
function Build-Test([string]$Name, [string[]]$Extra = @(), [string[]]$Libs = @()) {
    & cl.exe @flags "tests/$Name.cpp" @Extra /link "/OUT:$OutDir/$Name.exe" d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib psapi.lib user32.lib shell32.lib ole32.lib windowscodecs.lib advapi32.lib @Libs
    if ($LASTEXITCODE) { throw "$Name compilation failed" }
}
Build-Test 'forced_nr'
& "$OutDir/forced_nr.exe"
if ($LASTEXITCODE) {throw 'Forced NR policy/settings regression failed'}
Build-Test 'nr_smoke' @('src/core/DlssNrFilter.cpp','src/core/ColorGrading.cpp','src/core/OpticalFlow.cpp','src/core/OpticalFlowShaders.cpp',
    'src/core/ComputePasses.cpp','src/core/CommandListTracker.cpp','src/core/FreezeWatchdog.cpp') `
    @((Join-Path $DependencyRoot 'dlss/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib'),"$OutDir/ffx_optical.lib")
& "$OutDir/nr_smoke.exe" --forced-final --switch-scale --switch-layers --no-guides --colour=0
if ($LASTEXITCODE) {throw 'Forced final NR scale/layers/optical regression failed'}
& "$OutDir/nr_smoke.exe" --forced-final --switch-routes --switch-motion --switch-quality --switch-layers --colour=0
if ($LASTEXITCODE) {throw 'Forced final NR route restoration regression failed'}
