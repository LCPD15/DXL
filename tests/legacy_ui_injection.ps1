param([string]$CoreDir='', [ValidateSet('Both','OpenGL','D3D9','D3D12')][string]$Api='Both',
    [switch]$Baseline, [switch]$ChildWindow, [ValidateRange(1,5)][int]$Repeat=1, [switch]$ReShade)
$ErrorActionPreference='Stop'
if($Api -eq 'D3D12' -and ($Baseline -or $ChildWindow)){throw 'D3D12 fixture requires injection into a top-level window'}
if($Baseline -and $ReShade){throw 'ReShade fixture requires complete-core injection'}
. (Join-Path $PSScriptRoot '../scripts/paths.ps1')
if(!$CoreDir){$CoreDir=Join-Path $BuildRoot 'dxl-0.1-graphics-compat'}
$runRoot=Resolve-DxlOutput '' ('legacy-ui-e2e-'+$PID)
$stage=Join-Path $runRoot 'tool'
New-Item -ItemType Directory -Force -Path $stage|Out-Null
Copy-Item -LiteralPath (Join-Path $CoreDir 'DXL-core.dll') -Destination $stage
if($ReShade){
    Copy-Item -LiteralPath (Join-Path $CoreDir 'DXL-ReShade.dll') -Destination $stage
    $effects=Join-Path $stage 'post-processing'
    New-Item -ItemType Directory -Force -Path $effects | Out-Null
    Copy-Item -LiteralPath (Join-Path $SourceRoot 'post-processing/ReShade.fxh') -Destination $effects
    @'
#include "ReShade.fxh"
uniform float Strength < ui_type="slider"; ui_min=0.0; ui_max=1.0; > = 0.25;
float4 FixturePostProcess(float4 position:SV_Position,float2 uv:TEXCOORD):SV_Target {
    return tex2D(ReShade::BackBuffer,uv)+float4(Strength,Strength,Strength,0);
}
technique DxlUiCompatibility { pass { VertexShader=PostProcessVS; PixelShader=FixturePostProcess; } }
'@ | Set-Content -LiteralPath (Join-Path $effects '10-UI-Compatibility.fx') -Encoding utf8
    Get-FileHash -LiteralPath (Join-Path $stage 'DXL-ReShade.dll') -Algorithm SHA256 |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runRoot 'reshade-identity.json') -Encoding utf8
}
Get-FileHash -LiteralPath (Join-Path $stage 'DXL-core.dll') -Algorithm SHA256 |
    ConvertTo-Json|Set-Content -LiteralPath (Join-Path $runRoot 'core-identity.json') -Encoding utf8
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars)|ForEach-Object {
    if($_ -match '^([^=]+)=(.*)$'){Set-Item -LiteralPath ('Env:'+$matches[1]) -Value $matches[2]}
}
if ($Api -eq 'D3D12') {
    & cl.exe /nologo /LD /MT ('/Fo'+(Join-Path $runRoot 'fg-stub.obj')) ('/Fe'+(Join-Path $stage 'nvngx_dlssg.dll')) (Join-Path $PSScriptRoot 'fg_presence_stub.cpp') /link ('/IMPLIB:'+(Join-Path $runRoot 'fg-stub.lib'))
    if ($LASTEXITCODE) { throw 'FG presence stub build failed' }
}
$fixtureName='legacy-ui-'+$PID+'.exe'
$fixture=Join-Path $runRoot $fixtureName
$targetSource=if($Api -eq 'D3D12'){'late_ui_target.cpp'}else{'legacy_ui_target.cpp'}
& cl.exe /nologo /O2 /MT /EHsc /std:c++20 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN `
    ('/Fo'+(Join-Path $runRoot 'target.obj')) ('/Fe'+$fixture) (Join-Path $PSScriptRoot $targetSource)
if($LASTEXITCODE){throw 'Legacy UI target compilation failed'}
$profileRoot=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'DXL/profiles'
$profile=Join-Path $profileRoot ($fixtureName+'.json')
$params=Join-Path $profileRoot ($fixtureName+'.params.json')
$preset=Join-Path $profileRoot ($fixtureName+'.reshade.ini')
$reShadeRoot=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'DXL/ReShade'
$reShadeState=Join-Path $reShadeRoot $fixtureName
$ownedLogs=[Collections.Generic.List[string]]::new()
if((Test-Path -LiteralPath $profile) -or (Test-Path -LiteralPath $params) -or (Test-Path -LiteralPath $preset) -or (Test-Path -LiteralPath $reShadeState)){throw 'Unique UI fixture state exists'}
@{srEnable=$false;dlss5Enable=$false;masterEnabled=$false;diagEavesdrop=$true;diagNoUiPresent=$false;
    overlayVk=0;overlayMods=0}|ConvertTo-Json|Set-Content -LiteralPath $profile -Encoding utf8
try {
    foreach($run in 1..$Repeat){foreach($targetApi in $(if($Api -eq 'Both'){@('OpenGL','D3D9')}else{@($Api)})){
        $label=$targetApi+'-'+$run
        $stdout=Join-Path $runRoot ($label+'.stdout.log');$stderr=Join-Path $runRoot ($label+'.stderr.log')
        $loadArgument=if($Baseline){'--baseline'}else{'"'+(Join-Path $stage 'DXL-core.dll')+'"'}
        $targetArguments=@($loadArgument,$targetApi)
        if($ChildWindow){$targetArguments+='--child-window'}
        $process=$null
        try {
            $process=Start-Process -FilePath $fixture -WorkingDirectory $runRoot -ArgumentList $targetArguments `
                -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
            $null=$process.Handle
            Write-Output ('UI fixture '+$targetApi+' PID='+$process.Id+' output='+$runRoot)
            if(!$process.WaitForExit(40000)){throw 'Owned synthetic UI fixture exceeded its bound'}
            $process.Refresh()
            $coreLog=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) ('DXL/diagnostics/logs/DXL-core-'+$process.Id+'.log')
            if(Test-Path -LiteralPath $coreLog){Copy-Item -LiteralPath $coreLog -Destination (Join-Path $runRoot ($label+'.core.log'));$ownedLogs.Add($coreLog)}
            Get-Content -LiteralPath $stdout
            if ($Api -eq 'D3D12') {
                $persisted = Get-Content -LiteralPath $params -Raw | ConvertFrom-Json
                if ([Math]::Abs($persisted.nrSkinStructure - 0.37) -gt 0.001) { throw 'Core NR edit did not persist' }
                if ((Get-Content -LiteralPath $coreLog -Raw) -notmatch 'FG Present guard: active=1') { throw 'FG heuristic not reproduced' }
            }
            if($ReShade){
                $nativeLog=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) ('DXL/logs/DXL-ReShade-'+$process.Id+'.log')
                if(!(Test-Path -LiteralPath $nativeLog)){throw 'Private ReShade runtime log missing in full-core fixture'}
                Copy-Item -LiteralPath $nativeLog -Destination (Join-Path $runRoot ($label+'.reshade.log'));$ownedLogs.Add($nativeLog)
                if((Get-Content -LiteralPath $coreLog -Raw) -notmatch 'DXL ReShade runtime attached to final output'){throw 'Private runtime never attached through production hooks'}
                $nativeText=Get-Content -LiteralPath $nativeLog -Raw
                if($nativeText -notmatch 'Successfully compiled[^\r\n]*10-UI-Compatibility\.fx'){throw 'Native .fx did not compile through production hooks'}
                if($nativeText -match '\| ERROR \||Failed to compile'){throw 'Private runtime reported an effect error'}
                Write-Output ('PASS '+$targetApi+' production hooks attached private ReShade and compiled native .fx with overlay/input readback retained')
            }
            if($process.ExitCode -ne 0){throw ('Legacy '+$targetApi+' UI fixture failed: '+$process.ExitCode)}
        } finally {
            if($process){
                if(!$process.HasExited){
                    if(!$process.Path -or [IO.Path]::GetFullPath($process.Path) -ne [IO.Path]::GetFullPath($fixture)){throw 'Unexpected fixture process path during cleanup'}
                    $process.Kill()
                    if(!$process.WaitForExit(5000)){throw 'Owned synthetic fixture did not exit after termination'}
                }
                foreach($entry in @(
                    @{relative=('DXL/diagnostics/logs/DXL-core-'+$process.Id+'.log');suffix='.core.log'},
                    @{relative=('DXL/logs/DXL-ReShade-'+$process.Id+'.log');suffix='.reshade.log'})){
                    $owned=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) $entry.relative
                    if((Test-Path -LiteralPath $owned) -and !$ownedLogs.Contains($owned)){
                        Copy-Item -LiteralPath $owned -Destination (Join-Path $runRoot ($label+$entry.suffix)) -Force
                        $ownedLogs.Add($owned)
                    }
                }
                $process.Dispose()
            }
        }
    }}
    Write-Output ('PASS '+$Api+' UI-only framebuffer readback; baseline='+$Baseline+' repeats='+$Repeat+': '+$runRoot)
} finally {
    foreach($file in @($profile,$params,$preset)){
        $resolved=[IO.Path]::GetFullPath($file)
        if(!$resolved.StartsWith([IO.Path]::GetFullPath($profileRoot)+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid unique profile cleanup path'}
        if(Test-Path -LiteralPath $resolved){
            Copy-Item -LiteralPath $resolved -Destination (Join-Path $runRoot ([IO.Path]::GetFileName($resolved))) -Force
            Remove-Item -LiteralPath $resolved
        }
    }
    $resolvedState=[IO.Path]::GetFullPath($reShadeState)
    $resolvedRoot=[IO.Path]::GetFullPath($reShadeRoot)
    if(!$resolvedState.StartsWith($resolvedRoot+'\',[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolvedState) -ne $fixtureName){throw 'Invalid unique ReShade cache cleanup path'}
    if(Test-Path -LiteralPath $resolvedState){
        $stateItem=Get-Item -LiteralPath $resolvedState -Force
        if($stateItem.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Unexpected linked ReShade cache path'}
        Remove-Item -LiteralPath $resolvedState -Recurse -Force
    }
    foreach($file in $ownedLogs){
        $resolved=[IO.Path]::GetFullPath($file)
        $dxlRoot=[IO.Path]::GetFullPath((Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'DXL'))
        if(!$resolved.StartsWith($dxlRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid owned log cleanup path'}
        if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved -Force}
    }
}
