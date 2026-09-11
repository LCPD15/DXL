param([string]$CoreDir='', [ValidateSet('Both','OpenGL','D3D9')][string]$Api='Both',
    [switch]$Baseline, [ValidateRange(1,5)][int]$Repeat=1)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../scripts/paths.ps1')
if(!$CoreDir){$CoreDir=Join-Path $BuildRoot 'dxl-0.1-graphics-compat'}
$runRoot=Resolve-DxlOutput '' ('legacy-ui-e2e-'+$PID)
$stage=Join-Path $runRoot 'tool'
New-Item -ItemType Directory -Force -Path $stage|Out-Null
Copy-Item -LiteralPath (Join-Path $CoreDir 'DXL-core.dll') -Destination $stage
Get-FileHash -LiteralPath (Join-Path $stage 'DXL-core.dll') -Algorithm SHA256 |
    ConvertTo-Json|Set-Content -LiteralPath (Join-Path $runRoot 'core-identity.json') -Encoding utf8
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars)|ForEach-Object {
    if($_ -match '^([^=]+)=(.*)$'){Set-Item -LiteralPath ('Env:'+$matches[1]) -Value $matches[2]}
}
$fixtureName='legacy-ui-'+$PID+'.exe'
$fixture=Join-Path $runRoot $fixtureName
& cl.exe /nologo /O2 /MT /EHsc /std:c++20 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN `
    ('/Fo'+(Join-Path $runRoot 'target.obj')) ('/Fe'+$fixture) (Join-Path $PSScriptRoot 'legacy_ui_target.cpp')
if($LASTEXITCODE){throw 'Legacy UI target compilation failed'}
$profileRoot=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'DXL/profiles'
$profile=Join-Path $profileRoot ($fixtureName+'.json')
$params=Join-Path $profileRoot ($fixtureName+'.params.json')
if((Test-Path -LiteralPath $profile) -or (Test-Path -LiteralPath $params)){throw 'Unique UI fixture profile exists'}
@{srEnable=$false;dlss5Enable=$false;masterEnabled=$false;diagEavesdrop=$true;diagNoUiPresent=$false;
    overlayVk=0;overlayMods=0}|ConvertTo-Json|Set-Content -LiteralPath $profile -Encoding utf8
try {
    foreach($run in 1..$Repeat){foreach($targetApi in $(if($Api -eq 'Both'){@('OpenGL','D3D9')}else{@($Api)})){
        $label=$targetApi+'-'+$run
        $stdout=Join-Path $runRoot ($label+'.stdout.log');$stderr=Join-Path $runRoot ($label+'.stderr.log')
        $loadArgument=if($Baseline){'--baseline'}else{'"'+(Join-Path $stage 'DXL-core.dll')+'"'}
        $process=Start-Process -FilePath $fixture -WorkingDirectory $runRoot -ArgumentList @($loadArgument,$targetApi) `
            -WindowStyle Normal -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        Write-Output ('UI fixture '+$targetApi+' PID='+$process.Id+' output='+$runRoot)
        if(!$process.WaitForExit(40000)){
            $process.Refresh()
            if($process.Path -and [IO.Path]::GetFullPath($process.Path) -eq $fixture){$process.Kill()}
            throw 'Owned synthetic UI fixture exceeded its bound'
        }
        $process.Refresh()
        $coreLog=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) ('DXL/diagnostics/logs/DXL-core-'+$process.Id+'.log')
        if(Test-Path -LiteralPath $coreLog){Copy-Item -LiteralPath $coreLog -Destination (Join-Path $runRoot ($label+'.core.log'))}
        Get-Content -LiteralPath $stdout
        if($process.ExitCode -ne 0){throw ('Legacy '+$targetApi+' UI fixture failed: '+$process.ExitCode)}
    }}
    Write-Output ('PASS '+$Api+' UI-only framebuffer readback; baseline='+$Baseline+' repeats='+$Repeat+': '+$runRoot)
} finally {
    foreach($file in @($profile,$params)){
        $resolved=[IO.Path]::GetFullPath($file)
        if(!$resolved.StartsWith([IO.Path]::GetFullPath($profileRoot)+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid unique profile cleanup path'}
        if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved}
    }
}
