param([string]$OutDir = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
if (!$OutDir) { $OutDir = Join-Path $WorkspaceRoot ('diagnostics/startup-tests/run-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $PID) }
$OutDir = Resolve-DxlOutput $OutDir
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC x64 tools not found' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars) | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -LiteralPath ('Env:' + $matches[1]) -Value $matches[2] }
}
$flags = @('/nologo','/O2','/MT','/EHsc','/std:c++20','/utf-8','/W4','/DUNICODE','/D_UNICODE','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN')
Push-Location $SourceRoot
try {
    & cl.exe @flags tests/startup_diagnostics_fixture.cpp ("/Fo$OutDir/startup_diagnostics_fixture.obj") /link /DLL ("/OUT:$OutDir/startup_diagnostics_fixture.dll") ("/IMPLIB:$OutDir/startup_diagnostics_fixture.lib")
    if ($LASTEXITCODE) { throw 'Startup fixture DLL compilation failed' }
    & cl.exe @flags tests/startup_diagnostics.cpp ("/Fo$OutDir/startup_diagnostics.obj") /link ("/OUT:$OutDir/startup_diagnostics.exe")
    if ($LASTEXITCODE) { throw 'Startup diagnostics test compilation failed' }
    & "$OutDir/startup_diagnostics.exe" | Tee-Object -FilePath "$OutDir/startup-diagnostics.log"
    if ($LASTEXITCODE) { throw 'Startup diagnostics tests failed' }
} finally { Pop-Location }
Write-Output "PASS startup diagnostic artifacts: $OutDir"
