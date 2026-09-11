param([string]$OutDir='eastward-sync',[switch]$Matrix)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$OutDir=Resolve-DxlOutput $OutDir
Set-Location $SourceRoot
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars) | ForEach-Object {
    if($_ -match '^([^=]+)=(.*)$'){Set-Item -LiteralPath ('Env:'+$matches[1]) -Value $matches[2]}
}
& cl.exe /nologo /O2 /MT /EHsc /std:c++20 /utf-8 /W3 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN `
    tests/d3d11_fence_probe.cpp "/Fo$OutDir/d3d11_fence_probe.obj" /link "/OUT:$OutDir/d3d11_fence_probe.exe" d3d10.lib d3d11.lib d3d12.lib dxgi.lib dxguid.lib user32.lib
if($LASTEXITCODE){throw 'Fence probe compilation failed'}
$probeArgs=if($Matrix){@('--matrix')}else{@()}
Push-Location $OutDir
try { & "$OutDir/d3d11_fence_probe.exe" @probeArgs | Tee-Object -FilePath "$OutDir/fence-probe.log" } finally {Pop-Location}
if($LASTEXITCODE){throw 'Fence probe failed'}
