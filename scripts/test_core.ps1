param([string]$OutDir = '', [switch]$Smoke, [switch]$SmokeOnly, [switch]$NgxOnly, [switch]$SettingsOnly, [switch]$BridgeOnly)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$OutDir = Resolve-DxlOutput $OutDir
Set-Location (Split-Path $PSScriptRoot -Parent)
& ./scripts/build_core.ps1 -OutDir $OutDir -LibraryOnly
if ($Smoke -or $SmokeOnly -or $BridgeOnly) {
    # A fresh test directory must be runnable without a previous full build.
    $nrRuntime = Join-Path $DependencyRoot 'runtime/nvngx_dlssnr.dll'
    if (!(Test-Path -LiteralPath $nrRuntime -PathType Leaf)) { throw "NR test runtime missing: $nrRuntime" }
    $nrRuntimeDirectory = Join-Path $OutDir 'ngx'
    New-Item -ItemType Directory -Force -Path $nrRuntimeDirectory | Out-Null
    Copy-Item -LiteralPath $nrRuntime -Destination (Join-Path $nrRuntimeDirectory 'nvngx_dlssnr.dll') -Force
}
New-Item -ItemType Directory -Path "$OutDir/obj/tests" -Force | Out-Null
$flags = @('/nologo','/O2','/MT','/EHsc','/std:c++20','/utf-8','/W3','/D_CRT_SECURE_NO_WARNINGS',
    '/DUNICODE','/D_UNICODE','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/Isrc/core','/Isrc/common','/Ithird_party/minhook/include',
    ('/I'+(Join-Path $DependencyRoot 'dlss/include')),'/Ithird_party/fidelityfx/sdk/include','/Ithird_party/fidelityfx/sdk/src/backends/shared',"/Fo$OutDir/obj/tests/")
function Build-Test([string]$Name, [string[]]$Extra = @(), [string[]]$Libs = @()) {
    & cl.exe @flags "tests/$Name.cpp" @Extra /link "/OUT:$OutDir/$Name.exe" d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib psapi.lib user32.lib shell32.lib ole32.lib advapi32.lib @Libs
    if ($LASTEXITCODE) { throw "$Name compilation failed" }
}
if ($SettingsOnly -or (!$SmokeOnly -and !$NgxOnly -and !$BridgeOnly)) {
    Build-Test 'settings_ownership'
    Push-Location $OutDir
    try { & "$OutDir/settings_ownership.exe" } finally { Pop-Location }
    if ($LASTEXITCODE) { throw 'Settings ownership regression failed' }
    Build-Test 'game_compatibility'
    Push-Location $OutDir
    try { & "$OutDir/game_compatibility.exe" } finally { Pop-Location }
    if ($LASTEXITCODE) { throw 'Per-game compatibility defaults regression failed' }
    if ($SettingsOnly) { return }
}
if (!$SmokeOnly -and !$NgxOnly -and !$BridgeOnly) {
foreach ($name in @('presentation_owner','presentation_focus','direct_queue_candidate','nr_route','fg_swapchain','chain_child_policy')) {
    Build-Test $name
    & "$OutDir/$name.exe"
    if ($LASTEXITCODE) { throw "$name failed" }
}
& "$OutDir/nr_route.exe" --typeless-color
if ($LASTEXITCODE) { throw 'Typeless color route probe regression failed' }
Build-Test 'colour_blend' @('src/core/ComputePasses.cpp')
& "$OutDir/colour_blend.exe"
if ($LASTEXITCODE) { throw 'Colour regression failed' }
Build-Test 'typeless_colour' @('src/core/ComputePasses.cpp')
& "$OutDir/typeless_colour.exe"
if ($LASTEXITCODE) { throw 'Typeless color SRV regression failed' }
Build-Test 'gpu_compat' @('src/core/CommandListTracker.cpp','src/core/ComputePasses.cpp')
& "$OutDir/gpu_compat.exe"
if ($LASTEXITCODE) { throw 'GPU compatibility regression failed' }
Build-Test 'nr_slot_fence' @('src/core/DlssNrFilter.cpp','src/core/OpticalFlow.cpp','src/core/OpticalFlowShaders.cpp',
    'src/core/ComputePasses.cpp','src/core/CommandListTracker.cpp','src/core/FreezeWatchdog.cpp') `
    @((Join-Path $DependencyRoot 'dlss/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib'),"$OutDir/ffx_optical.lib")
& "$OutDir/nr_slot_fence.exe"
if ($LASTEXITCODE) { throw 'NR slot fence regression failed' }
}
if (!$SmokeOnly -and !$BridgeOnly) {
# NGX route fixture also tests driver-cached .bin model identity.
$fixtureRoot = Join-Path $OutDir 'ngx-fixtures/NVIDIA/NGX/models'
& cl.exe @flags tests/ngx_fixture.cpp /link /DLL "/OUT:$OutDir/ngx_fixture.dll"
if ($LASTEXITCODE) { throw 'NGX fixture compile failed' }
foreach ($kind in @('dlss','dlssd','dlssg')) {
    $dir = "$fixtureRoot/$kind/versions/20318464/files"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Copy-Item -LiteralPath "$OutDir/ngx_fixture.dll" -Destination "$dir/160_E658700.bin" -Force
}
Build-Test 'ngx_route' @('src/core/CommandListTracker.cpp','third_party/minhook/src/hook.c',
    'third_party/minhook/src/buffer.c','third_party/minhook/src/trampoline.c','third_party/minhook/src/hde/hde64.c')
Push-Location $OutDir
try { & "$OutDir/ngx_route.exe" } finally { Pop-Location }
if ($LASTEXITCODE) { throw 'NGX route regression failed' }
}
if ($Smoke -or $SmokeOnly) {
    Build-Test 'nr_smoke' @('src/core/DlssNrFilter.cpp','src/core/OpticalFlow.cpp','src/core/OpticalFlowShaders.cpp',
        'src/core/ComputePasses.cpp','src/core/CommandListTracker.cpp','src/core/FreezeWatchdog.cpp') `
        @((Join-Path $DependencyRoot 'dlss/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib'),"$OutDir/ffx_optical.lib")
    & "$OutDir/nr_smoke.exe" --switch-routes --switch-motion --switch-quality --switch-layers --colour=0
    if ($LASTEXITCODE) { throw 'NR/optical/layers integration regression failed' }
    & "$OutDir/nr_smoke.exe" --switch-scale --switch-layers --no-guides --colour=0
    if ($LASTEXITCODE) { throw 'NR scale/layer optical regression failed' }
    & "$OutDir/nr_smoke.exe" --typeless-color --switch-routes --switch-motion --switch-quality --switch-layers --colour=0
    if ($LASTEXITCODE) { throw 'Typeless NR route/optical/layers regression failed' }
    & "$OutDir/nr_smoke.exe" --typeless-color --switch-scale --switch-layers --no-guides --colour=0
    if ($LASTEXITCODE) { throw 'Typeless NR scale/layer optical regression failed' }
    & "$OutDir/nr_smoke.exe" --heapless-bindings --rgba8-color --direct-guides --colour=0
    if ($LASTEXITCODE) { throw 'Heapless real NR continuity regression failed' }
    & "$OutDir/nr_smoke.exe" --failure-streak --no-optical
    if ($LASTEXITCODE) { throw 'NR Evaluate consecutive-failure regression failed' }
    & "$OutDir/nr_smoke.exe" --failure-streak --present --no-optical
    if ($LASTEXITCODE) { throw 'NR Present consecutive-failure regression failed' }
}
if ($Smoke -or $SmokeOnly -or $BridgeOnly) {
    Build-Test 'nr_bridge11' @('src/core/DlssNrFilter11.cpp','src/core/DlssNrFilter.cpp','src/core/OpticalFlow.cpp',
        'src/core/OpticalFlowShaders.cpp','src/core/ComputePasses.cpp','src/core/CommandListTracker.cpp','src/core/FreezeWatchdog.cpp') `
        @('d3d10.lib','d3d11.lib',(Join-Path $DependencyRoot 'dlss/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib'),"$OutDir/ffx_optical.lib")
    & "$OutDir/nr_bridge11.exe" --failure-streak
    if ($LASTEXITCODE) { throw 'D3D11 NR bridge regression failed' }
    & "$OutDir/nr_bridge11.exe" --d3d10
    if ($LASTEXITCODE) { throw 'D3D10 NR bridge/state restoration regression failed' }
    & "$OutDir/nr_bridge11.exe" --d3d10 --single-threaded
    if ($LASTEXITCODE) { throw 'Single-threaded D3D10 NR bridge regression failed' }
}
