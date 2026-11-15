param([Parameter(Mandatory=$true)][string]$CoreDir,[ValidateSet('11','12')][string[]]$Api=@('11','12'),[switch]$TracePresent,[switch]$HalfSource)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$run=Resolve-DxlOutput '' ('fg-e2e-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+$PID)
New-Item -ItemType Directory -Path $run | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars) | ForEach-Object {
    if($_ -match '^([^=]+)=(.*)$'){Set-Item -LiteralPath ('Env:'+$matches[1]) -Value $matches[2]}
}
$flags=@('/nologo','/O2','/EHsc','/MT','/std:c++20','/utf-8','/DUNICODE','/D_UNICODE','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN')
$controller=Join-Path $run 'control.exe'
& cl.exe @flags ('/Fe'+$controller) ('/Fo'+(Join-Path $run 'control.obj')) (Join-Path $SourceRoot 'tests/fg_e2e_control.cpp') /link user32.lib gdi32.lib
if($LASTEXITCODE){throw 'FG controller build failed'}
$profiles=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'DXL/profiles'
New-Item -ItemType Directory -Path $profiles -Force | Out-Null
$failed=@()
foreach($a in $Api) {
    $stem='dxl-fg'+$a+'-probe-'+$PID
    $target=Join-Path $run ($stem+'.exe')
    $fixture=if($a -eq '12'){'testapp.cpp'}else{'testapp11.cpp'}
    & cl.exe @flags ('/Fe'+$target) ('/Fo'+(Join-Path $run ($stem+'.obj'))) (Join-Path $SourceRoot ('src/testapp/'+$fixture)) /link shell32.lib user32.lib
    if($LASTEXITCODE){throw "D3D$a fixture build failed"}
    $profile=Join-Path $profiles ($stem+'.exe.json')
    $params=Join-Path $profiles ($stem+'.exe.params.json')
    if((Test-Path -LiteralPath $profile) -or (Test-Path -LiteralPath $params)){throw "Existing fixture files at $profile"}
    $p=$null
    $trace="DXLFgTrace$PID-$a"
    $tracing=$false
    try {
        @{masterEnabled=$true;srEnable=$false;dlss5Enable=$false;nrProcessingEnabled=$false;diagEavesdrop=$false;dlss5AtEvaluate=$false;
            showToasts=$false;overlayVk=0;overlayMods=0} | ConvertTo-Json | Set-Content -LiteralPath $profile -Encoding utf8
        $arguments=if($a -eq '12'){'--fg-fixture --delay 3 --novsync'}else{'--fg-fixture --sync 0 --seconds 120'}
        if ($a -eq "12" -and $HalfSource) { $arguments += " --fg-half-source" }
        $p=Start-Process -FilePath $target -ArgumentList $arguments -WorkingDirectory $run -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $run "$a.host.log") -RedirectStandardError (Join-Path $run "$a.host.err.log")
        if($a -eq '11'){Start-Sleep -Milliseconds 800}
        & (Join-Path $CoreDir 'DXL-inject.exe') --pid $p.Id | Out-File -Encoding utf8 (Join-Path $run "$a.inject.log")
        if($LASTEXITCODE){throw "D3D$a injection failed"}
        if($TracePresent) {
            & logman start $trace -p 'Microsoft-Windows-DXGI' 0xffffffffffffffff 4 -ets -o (Join-Path $run "$a.dxgi.etl") | Out-Null
            $tracing=$LASTEXITCODE -eq 0
            if(!$tracing){throw 'Starting DXGI trace failed'}
        }
        & $controller $p.Id $a | Tee-Object -FilePath (Join-Path $run "$a.control.log")
        if($LASTEXITCODE){throw "D3D$a control failed"}
    } catch {
        $failed+=("D3D"+$a+": "+$_.Exception.Message)
    } finally {
        if($tracing){& logman stop $trace -ets | Out-Null}
        if($p){
            if(!$p.HasExited){$null=$p.CloseMainWindow();if(!$p.WaitForExit(4000)){$p.Kill();$p.WaitForExit()}}
            $coreLog=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) ('DXL/diagnostics/logs/DXL-core-'+$p.Id+'.log')
            if(Test-Path -LiteralPath $coreLog){Copy-Item -LiteralPath $coreLog -Destination (Join-Path $run "$a.core.log")}
            if($tracing){
                $xpath="*[System[(EventID=42) and Execution[@ProcessID="+$p.Id+"]]]"
                $groups=Get-WinEvent -Path (Join-Path $run "$a.dxgi.etl") -Oldest -FilterXPath $xpath -ErrorAction SilentlyContinue | 
                    Group-Object -Property {$_.Properties[0].Value} | Select-Object Name,Count
                $groups | Format-Table | Out-String | Tee-Object -FilePath (Join-Path $run "$a.presents.txt")
            }
            $p.Dispose()
        }
        foreach($file in @($params,$profile)){
            if(Test-Path -LiteralPath $file -PathType Leaf){
                $resolved=Get-Item -LiteralPath $file
                if($resolved.DirectoryName -ne $profiles -or $resolved.Name -notin @(($stem+'.exe.params.json'),($stem+'.exe.json')) -or $resolved.LinkType){throw 'Unexpected fixture path'}
                Copy-Item -LiteralPath $file -Destination $run
                Remove-Item -LiteralPath $file
            }
        }
    }
}
Write-Output ("Evidence: "+$run)
if($failed.Count){throw ($failed -join '; ')}

