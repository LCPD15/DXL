param([string]$OutDir = 'injection-diagnostics')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$testRoot = Resolve-DxlOutput $OutDir
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$visualStudio) { throw 'Visual Studio C++ tools were not found.' }
$vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
foreach ($line in (& $env:ComSpec /d /c ('"' + $vcvars + '" >nul && set'))) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
if ($LASTEXITCODE) { throw 'vcvars64.bat failed.' }
$flags = @('/nologo','/utf-8','/std:c++20','/EHsc','/O2','/MT','/W4','/permissive-',
    '/DUNICODE','/D_UNICODE','/DWIN32_LEAN_AND_MEAN','/DNOMINMAX')
$fixture = Join-Path $testRoot 'injection-diagnostic-fixture.dll'
$test = Join-Path $testRoot 'injection-diagnostics.exe'
& cl.exe @flags /LD (Join-Path $SourceRoot 'tests/injection_diagnostic_fixture.cpp') `
    ('/Fo' + (Join-Path $testRoot 'fixture.obj')) /link ('/OUT:' + $fixture) `
    ('/IMPLIB:' + (Join-Path $testRoot 'fixture.lib'))
if ($LASTEXITCODE) { throw 'Injection diagnostic fixture compile failed.' }
& cl.exe @flags (Join-Path $SourceRoot 'tests/injection_diagnostics.cpp') `
    ('/Fo' + (Join-Path $testRoot 'diagnostics.obj')) ('/Fe' + $test) /link user32.lib psapi.lib shell32.lib ole32.lib
if ($LASTEXITCODE) { throw 'Injection diagnostic test compile failed.' }
& $test $fixture
if ($LASTEXITCODE) { throw 'Injection diagnostic tests failed.' }
