param([string]$OutDir = '', [switch]$SkipAcquire)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$OutDir = Resolve-DxlOutput $OutDir 'reshade-6.8.0'
$dependency = Join-Path $DependencyRoot 'reshade'
$source = Join-Path $dependency 'reshade-18deaa52de0c425a78b329e9cb3c497281cd00ec'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC x64 tools not found' }
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (!(Test-Path -LiteralPath $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }
$python = (Get-Command python -ErrorAction Stop).Source
if (!$SkipAcquire) {
    & $python (Join-Path $PSScriptRoot 'prepare_reshade_runtime.py') $dependency
    if ($LASTEXITCODE) { throw 'ReShade pinned source preparation failed' }
}
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars) | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -LiteralPath ('Env:'+$matches[1]) -Value $matches[2] }
}
$fxc = Join-Path $env:WindowsSdkVerBinPath 'x64/fxc.exe'
foreach ($shader in @(@('copy_ps','ps_4_0'),@('fullscreen_vs','vs_4_0'),@('imgui_ps_3_0','ps_3_0'),@('imgui_ps_4_0','ps_4_0'),@('imgui_vs_3_0','vs_3_0'),@('imgui_vs_4_0','vs_4_0'),@('mipmap_cs_5_0','cs_5_0'))) {
    & $fxc /nologo /O3 /T $shader[1] /E main /Fo (Join-Path $source ('res/shaders/'+$shader[0]+'.cso')) (Join-Path $source ('res/shaders/'+$shader[0]+'.hlsl'))
    if ($LASTEXITCODE) { throw ('ReShade resource shader failed: '+$shader[0]) }
}
$vsVersion = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
$generator = if ([int]($vsVersion.Split('.')[0]) -ge 18) { 'Visual Studio 18 2026' } else { 'Visual Studio 17 2022' }
& $cmake -S $source -B $OutDir -G $generator -A x64 "-DPython_EXECUTABLE=$python"
if ($LASTEXITCODE) { throw 'ReShade CMake configuration failed' }
& $cmake --build $OutDir --config Release --target ReShade --parallel 8
if ($LASTEXITCODE) { throw 'ReShade runtime compilation failed' }
$runtime = Join-Path $OutDir 'Release/DXL-ReShade.dll'
if (!(Test-Path -LiteralPath $runtime)) { throw 'Private ReShade runtime not found' }
New-Item -ItemType Directory -Path (Join-Path $dependency 'runtime') -Force | Out-Null
Copy-Item -LiteralPath $runtime -Destination (Join-Path $dependency 'runtime/DXL-ReShade.dll') -Force
Get-FileHash -LiteralPath $runtime -Algorithm SHA256
