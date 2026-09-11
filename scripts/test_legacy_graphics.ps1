param([string]$OutDir = '', [switch]$BuildOnly, [switch]$NativeD3D9, [switch]$NativeD3D9Ex, [switch]$D3D9Probe)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$OutDir = Resolve-DxlOutput $OutDir 'legacy-graphics'
Set-Location -LiteralPath $SourceRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC x64 tools not found' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars) | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -LiteralPath ('Env:'+$matches[1]) -Value $matches[2] }
}
New-Item -ItemType Directory -Force -Path "$OutDir/obj" | Out-Null
$flags = @('/nologo','/O2','/MT','/EHsc','/std:c++20','/utf-8','/W3','/DUNICODE','/D_UNICODE',
    '/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/Isrc/core','/Ithird_party/minhook/include',"/Fo$OutDir/obj/")
# The fixture includes LegacyGraphics.cpp so it can inspect a processed
# backbuffer before native Present discards it. Do not link a second copy.
& cl.exe @flags tests/legacy_graphics.cpp third_party/minhook/src/hook.c third_party/minhook/src/buffer.c `
    third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde64.c /link "/OUT:$OutDir/legacy_graphics.exe" `
    d3d11.lib dxgi.lib dxguid.lib user32.lib gdi32.lib shell32.lib advapi32.lib
if ($LASTEXITCODE) { throw 'Legacy bridge fixture compilation failed' }
& cl.exe @flags tests/legacy_gl_target.cpp /link "/OUT:$OutDir/legacy_gl_target.exe"
if ($LASTEXITCODE) { throw 'OpenGL injection target compilation failed' }
& cl.exe @flags tests/legacy_d3d9_target.cpp /link "/OUT:$OutDir/legacy_d3d9_target.exe"
if ($LASTEXITCODE) { throw 'D3D9Ex injection target compilation failed' }
if ($BuildOnly) { return }
function Run-LegacyTest([string]$Name, [string[]]$Arguments = @()) {
    Push-Location -LiteralPath $OutDir
    try { & "$OutDir/legacy_graphics.exe" @Arguments | Tee-Object -FilePath "$OutDir/$Name.log" }
    finally { Pop-Location }
    if ($LASTEXITCODE) { throw "$Name failed with exit code $LASTEXITCODE" }
}
if ($D3D9Probe) { Run-LegacyTest 'native-d3d9-environment' @('--probe9'); return }
Run-LegacyTest 'opengl-transfer'
Run-LegacyTest 'd3d9-controlled-transfer' @('--d3d9-transfer')
if ($NativeD3D9) {
    Run-LegacyTest 'd3d9-native-transfer' @('--d3d9')
} else {
    Write-Host 'Native D3D9 is not covered by controlled surfaces. Use -NativeD3D9 to test native API presentation on a compatible host.'
}
if ($NativeD3D9Ex) {
    Run-LegacyTest 'd3d9ex-native-transfer' @('--d3d9ex')
    Run-LegacyTest 'd3d9ex-late-transfer' @('--d3d9ex-late')
    Run-LegacyTest 'd3d9ex-msaa-transfer' @('--d3d9ex-msaa')
}
