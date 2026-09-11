param([string]$CoreDir='', [string]$Target='', [ValidateSet('OpenGL','D3D9')][string]$Api='OpenGL',
    [ValidateRange(0,1100)][int]$LoadAt=0, [ValidateSet(0,2,4,8)][int]$Msaa=0, [switch]$Cycle)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../scripts/paths.ps1')
if(!$CoreDir){$CoreDir=Join-Path $BuildRoot 'dxl-0.1-graphics-compat'}
if(!$Target){$Target=Join-Path $BuildRoot $(if($Api -eq 'D3D9'){'legacy-agent/legacy_d3d9_target.exe'}else{'legacy-agent/legacy_gl_target.exe'})}
$runRoot=Resolve-DxlOutput '' ('legacy-nr-e2e-'+$PID)
$stage=Join-Path $runRoot 'tool'
New-Item -ItemType Directory -Force -Path $stage,(Join-Path $stage 'ngx')|Out-Null
Copy-Item -LiteralPath (Join-Path $CoreDir 'DXL-core.dll') -Destination $stage -Force
Get-FileHash -LiteralPath (Join-Path $stage 'DXL-core.dll') -Algorithm SHA256 |
    ConvertTo-Json|Set-Content -LiteralPath (Join-Path $runRoot 'core-identity.json') -Encoding utf8
foreach($name in @('nvngx_dlss.dll','nvngx_dlssnr.dll')) {
    Copy-Item -LiteralPath (Join-Path $DependencyRoot ('runtime/'+$name)) -Destination (Join-Path $stage ('ngx/'+$name)) -Force
}
$fixtureName='legacy-nr-'+$PID+'.exe'
$fixture=Join-Path $runRoot $fixtureName
Copy-Item -LiteralPath $Target -Destination $fixture -Force
$profileRoot=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'DXL/profiles'
$profile=Join-Path $profileRoot ($fixtureName+'.json')
if(Test-Path -LiteralPath $profile){throw 'Unique legacy fixture profile exists'}
@{srEnable=$false;dlss5Enable=$true;masterEnabled=$true;diagEavesdrop=$true;dlss5AtEvaluate=$true;nrAutoRoute=$true;
    nrRenderScale=0.5;nrOpticalFlow=$true;nrOpticalFlowQuality=0;nrSelfLayers=1;nrTrueLayers=1;
    nrColourStrength=0;nrSemanticMask=$false;diagNoUiPresent=$true;overlayVk=0;overlayMods=0
}|ConvertTo-Json|Set-Content -LiteralPath $profile -Encoding utf8
try {
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs=& $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    & cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars)|ForEach-Object {
        if($_ -match '^([^=]+)=(.*)$'){Set-Item -LiteralPath ('Env:'+$matches[1]) -Value $matches[2]}
    }
    $probe=Join-Path $runRoot 'legacy_status_probe.exe'
    & cl.exe /nologo /O2 /MT /EHsc /std:c++20 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN `
        ('/Fo'+(Join-Path $runRoot 'probe.obj')) ('/Fe'+$probe) (Join-Path $PSScriptRoot 'legacy_status_probe.cpp')
    if($LASTEXITCODE){throw 'Legacy status probe compilation failed'}
    $stdout=Join-Path $runRoot 'target.stdout.log';$stderr=Join-Path $runRoot 'target.stderr.log'
    $arguments=@(('"--load='+(Join-Path $stage 'DXL-core.dll')+'"'),'--frames=1200','--resize')
    if($LoadAt){$arguments+=('--load-at='+$LoadAt)}
    if($Msaa){
        if($Api -ne 'D3D9'){throw 'The MSAA target option requires D3D9'}
        $arguments+=('--msaa='+$Msaa)
    }
    $process=Start-Process -FilePath $fixture -WorkingDirectory $runRoot -ArgumentList $arguments -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    Write-Output ('Legacy fixture pid='+$process.Id+' output='+$runRoot)
    $probeArgs=@([string]$process.Id,'45',$(if($Cycle){'--cycle'}else{'--no-cycle'}),$(if($Api -eq 'D3D9'){'9'}else{'30'}))
    & $probe @probeArgs | Tee-Object -FilePath (Join-Path $runRoot 'status.jsonl')
    $probeCode=$LASTEXITCODE
    if(!$process.WaitForExit(5000)){
        $actualPath=$process.Path
        if($actualPath -and [IO.Path]::GetFullPath($actualPath) -eq $fixture){$process.Kill()}
        throw 'Owned synthetic legacy fixture exceeded its bound'
    }
    $process.Refresh()
    $coreLog=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) ('DXL/diagnostics/logs/DXL-core-'+$process.Id+'.log')
    if(Test-Path -LiteralPath $coreLog){Copy-Item -LiteralPath $coreLog -Destination (Join-Path $runRoot 'core.log') -Force}
    Get-Content -LiteralPath $stdout|Select-Object -Last 5
    if($process.ExitCode -ne 0 -or $probeCode -ne 0){throw ('Integrated '+$Api+' NR fixture failed')}
    $completion=if($Api -eq 'D3D9'){'DONE presents=1200 D3D9_errors=0'}else{'DONE presents=1200 GL_errors=0'}
    if(!(Select-String -LiteralPath $stdout -Pattern $completion -Quiet)){throw ($Api+' target did not complete cleanly')}
    Write-Output ('PASS integrated native '+$Api+' -> D3D11/D3D12 NR, resize and bounded normal teardown: '+$runRoot)
} finally {
    $resolved=[IO.Path]::GetFullPath($profile)
    if(!$resolved.StartsWith([IO.Path]::GetFullPath($profileRoot)+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid unique profile cleanup path'}
    if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved}
}
