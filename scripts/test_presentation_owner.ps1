param([Parameter(Mandatory=$true)][string]$CoreDir, [string]$OutDir='', [switch]$ExpectOwnerFailure)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$CoreDir=Resolve-DxlOutput $CoreDir
$runRoot=Resolve-DxlOutput $OutDir ('presentation-owner-'+$PID)
$stage=Join-Path $runRoot 'tool'
New-Item -ItemType Directory -Force -Path $stage,(Join-Path $stage 'ngx')|Out-Null
Copy-Item -LiteralPath (Join-Path $CoreDir 'DXL-core.dll') -Destination $stage -Force
Get-FileHash -LiteralPath (Join-Path $stage 'DXL-core.dll') -Algorithm SHA256 |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runRoot 'core-identity.json') -Encoding utf8
Copy-Item -LiteralPath (Join-Path $DependencyRoot 'runtime/nvngx_dlssnr.dll') -Destination (Join-Path $stage 'ngx') -Force

$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'MSVC x64 tools not found'}
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars) | ForEach-Object {
    if($_ -match '^([^=]+)=(.*)$'){Set-Item -LiteralPath ('Env:'+$matches[1]) -Value $matches[2]}
}
$fixtureName='dxl-presentation-owner-'+$PID+'.exe'
$fixture=Join-Path $runRoot $fixtureName
& cl.exe /nologo /utf-8 /std:c++20 /EHsc /O2 /MT /W3 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX `
    ('/Fo'+(Join-Path $runRoot 'target.obj')) ('/Fe'+$fixture) (Join-Path $SourceRoot 'tests/presentation_owner_target.cpp') `
    /link /SUBSYSTEM:CONSOLE d3d11.lib d3d12.lib dxgi.lib user32.lib
if($LASTEXITCODE){throw 'Presentation owner target compile failed'}

# Only this run's unique profile is temporarily registered where the core reads
# profiles. Its source and every retained test artifact stay in the workspace.
$profileRoot=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'DXL/profiles'
$profile=Join-Path $profileRoot ($fixtureName+'.json')
$params=Join-Path $profileRoot ($fixtureName+'.params.json')
if((Test-Path -LiteralPath $profile) -or (Test-Path -LiteralPath $params)){throw 'Unique fixture profile already exists'}
$settings=@{srEnable=$false;dlss5Enable=$true;masterEnabled=$true;diagEavesdrop=$true;
    dlss5AtEvaluate=$true;nrAutoRoute=$true;nrRenderScale=0.5;nrOpticalFlow=$false;
    nrSelfLayers=1;nrTrueLayers=1;nrSemanticMask=$false;diagNoUiPresent=$false;overlayVk=0;overlayMods=0}
$profileSource=Join-Path $runRoot 'profile.json'
$settings | ConvertTo-Json | Set-Content -LiteralPath $profileSource -Encoding utf8
New-Item -ItemType Directory -Force -Path $profileRoot|Out-Null
Copy-Item -LiteralPath $profileSource -Destination $profile
try {
    $stdout=Join-Path $runRoot 'target.stdout.log'
    $stderr=Join-Path $runRoot 'target.stderr.log'
    $process=Start-Process -FilePath $fixture -WorkingDirectory $runRoot `
        -ArgumentList ('"'+(Join-Path $stage 'DXL-core.dll')+'"') -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    # Retain the owned process handle before it exits (also works in Windows PowerShell 5).
    $null=$process.Handle
    Write-Output ('Presentation fixture PID='+$process.Id+' output='+$runRoot)
    if(!$process.WaitForExit(60000)){
        $process.Refresh()
        if($process.Path -and [IO.Path]::GetFullPath($process.Path) -eq $fixture){$process.Kill()}
        throw 'Owned synthetic presentation fixture exceeded its timeout'
    }
    $fixtureExit=$process.ExitCode
    $logRoot=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'DXL/diagnostics/logs'
    $coreLog=Join-Path $logRoot ('DXL-core-'+$process.Id+'.log')
    foreach($suffix in @('','.exit')){
        if(Test-Path -LiteralPath ($coreLog+$suffix)){
            Copy-Item -LiteralPath ($coreLog+$suffix) -Destination (Join-Path $runRoot ('core.log'+$suffix))
        }
    }
    Get-Content -LiteralPath $stdout | Select-Object -Last 15
    if(!(Select-String -LiteralPath $stdout -Pattern 'OWNER_HELPER .*size=186x17 presents=12' -Quiet)){throw 'Tiny helper was not exercised'}
    if(!(Select-String -LiteralPath $stdout -Pattern 'OWNER_DEBUG enabled=1' -Quiet)){throw 'D3D12 validation unavailable'}
    if(Select-String -LiteralPath $stdout -Pattern '\[D3D12 (ERROR|CORRUPTION)\]' -Quiet){throw 'D3D12 validation error'}
    if($ExpectOwnerFailure){
        if($fixtureExit -ne 2 -or !(Select-String -LiteralPath $stdout -Pattern 'OWNER_STATUS passed=0 api=11 .*presents=12 .*mainUi=0 helperUi=1' -Quiet)){
            throw ('Expected the old core to retain only the D3D11 helper and skip the D3D12 main window; exit='+$fixtureExit)
        }
        Write-Output ('PASS expected old-core tiny-helper ownership failure: '+$runRoot)
    }else{
        if($fixtureExit -ne 0 -or !(Select-String -LiteralPath $stdout -Pattern 'OWNER_STATUS passed=1 .*mainUi=1 helperUi=0' -Quiet)){
            throw ('D3D12 presentation owner regression failed: exit='+$fixtureExit)
        }
        Write-Output ('PASS tiny D3D11 helper -> D3D12 owner, real NR, correct ImGui window and clean D3D12 validation: '+$runRoot)
    }
} finally {
    foreach($file in @($profile,$params)){
        $resolved=[IO.Path]::GetFullPath($file)
        if(!$resolved.StartsWith([IO.Path]::GetFullPath($profileRoot)+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid fixture profile cleanup path'}
        if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved}
    }
}
