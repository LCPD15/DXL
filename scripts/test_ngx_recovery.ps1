param([string]$OutDir = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$OutDir = Resolve-DxlOutput $OutDir 'ngx-recovery-tests-0.1'
Set-Location $SourceRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC x64 tools not found' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars) | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -LiteralPath ('Env:'+$matches[1]) -Value $matches[2] }
}
New-Item -ItemType Directory -Force -Path "$OutDir/obj" | Out-Null
$flags=@('/nologo','/O2','/MT','/EHsc','/std:c++20','/utf-8','/W3','/D_CRT_SECURE_NO_WARNINGS',
    '/DUNICODE','/D_UNICODE','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/Isrc/core','/Isrc/common',
    ('/I'+(Join-Path $DependencyRoot 'dlss/include')),'/Ithird_party/minhook/include',"/Fo$OutDir/obj/")
& cl.exe @flags tests/ngx_recovery_fixture.cpp /link /DLL "/OUT:$OutDir/ngx_recovery_fixture.dll"
if ($LASTEXITCODE) { throw 'Recovery fixture compilation failed' }
foreach ($name in @('nvngx_dlss.dll','nvngx_dlssg.dll','unrelated.dll')) {
    Copy-Item -LiteralPath "$OutDir/ngx_recovery_fixture.dll" -Destination "$OutDir/$name" -Force
}
foreach ($kind in @('dlss','dlssd','dlssg')) {
    $folder="$OutDir/NVIDIA/NGX/models/$kind/versions/1/files"
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    Copy-Item -LiteralPath "$OutDir/ngx_recovery_fixture.dll" -Destination "$folder/model.bin" -Force
}
New-Item -ItemType Directory -Force -Path "$OutDir/forwarded" | Out-Null
& link.exe /nologo /DLL /NOENTRY /MACHINE:X64 /DEF:tests/ngx_recovery_forwarded.def "/OUT:$OutDir/forwarded/nvngx_dlssd.dll"
if ($LASTEXITCODE) { throw 'Forwarded recovery fixture link failed' }
$minhook=@('third_party/minhook/src/hook.c','third_party/minhook/src/buffer.c',
    'third_party/minhook/src/trampoline.c','third_party/minhook/src/hde/hde64.c')
& cl.exe @flags tests/ngx_recovery.cpp src/core/CommandListTracker.cpp @minhook /link "/OUT:$OutDir/ngx_recovery.exe" `
    d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib psapi.lib user32.lib shell32.lib ole32.lib advapi32.lib
if ($LASTEXITCODE) { throw 'Recovery test compilation failed' }
& "$OutDir/ngx_recovery.exe"
if ($LASTEXITCODE) { throw 'NGX cached-export recovery regression failed' }
$negative = & "$OutDir/ngx_recovery.exe" --negative-no-recovery 2>&1
if ($LASTEXITCODE -ne 1 -or "$negative" -notmatch 'missing or duplicate wrapper invocation') {
    throw 'Recovery negative control did not detect cached-pointer loss'
}
Write-Output 'PASS negative control detects cached-pointer loss with recovery disabled'

# The existing early loader chain must keep forwarding through its original
# IAT wrappers; recovery does not require that otherwise-working route to change.
$chainDir=Join-Path $OutDir 'loader-chain'
New-Item -ItemType Directory -Force -Path $chainDir | Out-Null
& cl.exe @flags /DFIXTURE_UNITY tests/ngx_loader_fixture.cpp /link /DLL "/OUT:$chainDir/UnityPlayer.dll"
if ($LASTEXITCODE) { throw 'Unity loader fixture compilation failed' }
& cl.exe @flags tests/ngx_loader_fixture.cpp /link /DLL "/OUT:$chainDir/sl.common.dll"
if ($LASTEXITCODE) { throw 'Streamline loader fixture compilation failed' }
foreach ($name in @('nvngx_dlss.dll','nvngx_dlssg.dll')) {
    Copy-Item -LiteralPath "$OutDir/ngx_recovery_fixture.dll" -Destination "$chainDir/$name" -Force
}
& cl.exe @flags tests/ngx_loader_chain.cpp src/core/CommandListTracker.cpp @minhook /link "/OUT:$chainDir/ngx_loader_chain.exe" `
    "$chainDir/UnityPlayer.lib" d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib psapi.lib user32.lib shell32.lib ole32.lib advapi32.lib
if ($LASTEXITCODE) { throw 'Early loader-chain regression compilation failed' }
& "$chainDir/ngx_loader_chain.exe"
if ($LASTEXITCODE) { throw 'Early loader-chain regression failed' }
& cl.exe @flags tests/chain_child_policy.cpp /link "/OUT:$OutDir/chain_child_policy.exe" shell32.lib ole32.lib
if ($LASTEXITCODE) { throw 'Child crash-reporter policy regression compilation failed' }
& "$OutDir/chain_child_policy.exe"
if ($LASTEXITCODE) { throw 'Child crash-reporter policy regression failed' }
