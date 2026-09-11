param([string]$BuildDir = '', [switch]$WithoutSrRuntime)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$projectRoot = $SourceRoot
Set-Location -LiteralPath $projectRoot
$BuildDir = Resolve-DxlOutput $BuildDir
$testRoot = Resolve-DxlOutput '' ('e2e-0.1-'+$PID)
$toolDir = Join-Path $testRoot 'tool'
$gameDir = Join-Path $testRoot 'game'
$ownerDir = Join-Path $testRoot 'ownership'
New-Item -ItemType Directory -Force -Path $toolDir,$gameDir,$ownerDir,(Join-Path $toolDir 'ngx'),(Join-Path $toolDir 'shell') | Out-Null
foreach ($name in @('DXL-core.dll','DXL-inject.exe','DXL.exe')) {
    Copy-Item -LiteralPath (Join-Path $BuildDir $name) -Destination (Join-Path $toolDir $name) -Force
}
foreach ($name in @('d3d12','d3d11','dxgi','xinput1_4')) {
    Copy-Item -LiteralPath (Join-Path $BuildDir ('shell/' + $name + '.dll')) -Destination (Join-Path $toolDir ('shell/' + $name + '.dll')) -Force
}
foreach ($name in @('nvngx_dlss.dll','nvngx_dlssnr.dll','nvngx_dlssg.dll')) {
    if ($WithoutSrRuntime -and $name -eq 'nvngx_dlss.dll') { continue }
    $dest = Join-Path $toolDir ('ngx/' + $name)
    $source = Join-Path $DependencyRoot ('runtime/' + $name)
    if (!(Test-Path -LiteralPath $dest) -or (Get-Item -LiteralPath $dest).Length -ne (Get-Item -LiteralPath $source).Length) {
        Copy-Item -LiteralPath $source -Destination $dest -Force
    }
}
if ($WithoutSrRuntime -and (Test-Path -LiteralPath (Join-Path $toolDir 'ngx/nvngx_dlss.dll'))) { throw 'SR-free fixture contains the SR runtime' }
# Isolated fixture only; no existing profile directory is written.
$settings = @{
    srEnable=$false; dlss5Enable=$true; masterEnabled=$true; diagEavesdrop=$true
    dlss5AtEvaluate=$true; nrAutoRoute=$true; nrRenderScale=0.5
    nrOpticalFlow=$true; nrOpticalFlowQuality=0; nrSelfLayers=1.37; nrTrueLayers=2
    nrColourStrength=0.0; nrSemanticMask=$false; overlayVk=0; overlayMods=0
}
$profileRoot=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'DXL/profiles'
New-Item -ItemType Directory -Force -Path $profileRoot|Out-Null
$fixtureName='dxl-e2e-'+$PID+'.exe'
$fixtureProfile=Join-Path $profileRoot ($fixtureName+'.json')
if(Test-Path -LiteralPath $fixtureProfile){throw 'Fixture profile already exists'}
$settings | ConvertTo-Json | Set-Content -LiteralPath $fixtureProfile -Encoding utf8
try {

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
foreach ($line in (& $env:ComSpec /d /c ('"' + $vcvars + '" >nul && set'))) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$target = Join-Path $gameDir $fixtureName
& cl.exe /nologo /utf-8 /std:c++20 /EHsc /O2 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX `
    ('/Fo' + (Join-Path $testRoot 'target.obj')) ('/Fe' + $target) tests/test-dxl-e2e-target.cpp `
    /link /SUBSYSTEM:CONSOLE d3d12.lib dxgi.lib user32.lib
if ($LASTEXITCODE) { throw 'E2E target compile failed' }
$injector = Join-Path $toolDir 'DXL-inject.exe'
& $injector --deploy $target
if ($LASTEXITCODE) { throw 'Automatic shell selection/deployment failed' }
if (!(Test-Path -LiteralPath (Join-Path $gameDir 'd3d12.dll'))) { throw 'Expected d3d12 proxy from target imports' }
$marker = (Get-Content -LiteralPath (Join-Path $gameDir 'd5q-deploy.txt') -Raw).Trim()
if ($marker -ne (Join-Path $toolDir 'DXL-core.dll')) { throw 'Deployment marker did not point to central tool core' }

$stdout = Join-Path $testRoot 'target.stdout.log'
$stderr = Join-Path $testRoot 'target.stderr.log'
$fixture = Start-Process -FilePath $target -WorkingDirectory $gameDir -ArgumentList '--novsync --debug' -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
if (!$fixture.WaitForExit(45000)) {
    # Only the exact fixture created above may be ended. It has its own WM_CLOSE
    # timeout; a forced stop here is failure cleanup, never a passing result.
    $actualPath = $fixture.Path
    if ($actualPath -and [IO.Path]::GetFullPath($actualPath) -eq $target) { $fixture.Kill() }
    throw "E2E fixture failed to exit; inspect $stdout"
}
$fixture.Refresh()
Get-Content -LiteralPath $stdout | Select-Object -Last 12
$fixtureCode = $fixture.ExitCode
& $injector --undeploy $target
if ($LASTEXITCODE) { throw 'Fixture undeploy failed' }
if ((Test-Path -LiteralPath (Join-Path $gameDir 'd3d12.dll')) -or (Test-Path -LiteralPath (Join-Path $gameDir 'd5q-deploy.txt'))) { throw 'Owned deployment files remained' }
if ($fixtureCode -ne 0) { throw "E2E fixture returned $fixtureCode" }
if (!(Select-String -LiteralPath $stdout -Pattern 'E2E_STATUS passed=1' -Quiet)) { throw 'No passing IPC result' }
if ($WithoutSrRuntime -and !(Select-String -LiteralPath $stdout -Pattern 'E2E_SR_RUNTIME loaded=0' -Quiet)) { throw 'SR-free NR test loaded an SR runtime' }
if (!(Select-String -LiteralPath $stdout -Pattern 'E2E_DEBUG enabled=1' -Quiet)) { throw 'D3D12 debug layer was unavailable' }
if (Select-String -LiteralPath $stdout -Pattern '\[D3D12 (ERROR|CORRUPTION)\]' -Quiet) { throw 'D3D12 validation reported an error' }

# Foreign-plugin protection is tested in a separate directory, never loaded.
$ownerTarget = Join-Path $ownerDir 'dxl-e2e-target.exe'
Copy-Item -LiteralPath $target -Destination $ownerTarget -Force
$foreign = Join-Path $ownerDir 'dxgi.dll'
[IO.File]::WriteAllBytes($foreign, [Text.Encoding]::ASCII.GetBytes('DXL E2E foreign plugin ownership sentinel'))
$beforeHash = (Get-FileHash -LiteralPath $foreign).Hash
& $injector --deploy $ownerTarget dxgi
if ($LASTEXITCODE -eq 0) { throw 'Deployer unexpectedly accepted occupied foreign slot' }
& $injector --deploy $ownerTarget
if ($LASTEXITCODE) { throw 'Auto selection failed with foreign dxgi plugin present' }
& $injector --undeploy $ownerTarget
if ($LASTEXITCODE) { throw 'Ownership fixture undeploy failed' }
if (!(Test-Path -LiteralPath $foreign) -or (Get-FileHash -LiteralPath $foreign).Hash -ne $beforeHash) { throw 'Foreign plugin changed' }
Write-Output 'PASS automatic shell selection, central-core early load, Present NR + optical flow + two true layers, IPC and foreign-plugin-preserving undeploy'

} finally {
    # Only this run's unique fixture profile; never remove real game settings.
    $resolved=[IO.Path]::GetFullPath($fixtureProfile)
    if(!$resolved.StartsWith([IO.Path]::GetFullPath($profileRoot)+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid fixture cleanup path'}
    if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved}
}
