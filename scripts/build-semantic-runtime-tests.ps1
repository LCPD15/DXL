param([string]$OutDir = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$projectRoot = $SourceRoot
$output = Resolve-DxlOutput $OutDir 'semantic-tests-0.1'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
foreach ($line in (& $env:ComSpec /d /c ('"' + $vcvars + '" >nul && set'))) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$flags = @('/nologo','/utf-8','/std:c++20','/EHsc','/O2','/MT','/W4','/DUNICODE','/D_UNICODE','/DWIN32_LEAN_AND_MEAN','/DNOMINMAX')
& cl.exe @flags ('/Fo' + (Join-Path $output 'selection.obj')) ('/Fe' + (Join-Path $output 'selection.exe')) `
    (Join-Path $projectRoot 'tests/segmentation_runtime_selection.cpp')
if ($LASTEXITCODE) { throw 'Runtime selection test compilation failed.' }
& (Join-Path $output 'selection.exe')
if ($LASTEXITCODE) { throw 'Runtime selection test failed.' }
& cl.exe @flags /I (Join-Path $projectRoot 'src/core') /I (Join-Path $projectRoot 'third_party/tensorrt/include') `
    /I (Join-Path $projectRoot 'third_party/cuda-stub') ('/Fo' + $output + '\') `
    ('/Fe' + (Join-Path $output 'semantic_capture.exe')) (Join-Path $projectRoot 'tests/semantic_capture.cpp') `
    (Join-Path $projectRoot 'src/core/SegMaskFilter.cpp') /link d3d12.lib dxgi.lib dxguid.lib user32.lib gdi32.lib ole32.lib shell32.lib
if ($LASTEXITCODE) { throw 'Production segmentation runtime test compilation failed.' }
Write-Output "PASS production SegMaskFilter test compiled: $output (GPU execution is intentionally separate)"
