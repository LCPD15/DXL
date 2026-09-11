param([Parameter(Mandatory=$true)][string]$OutDir,[Parameter(Mandatory=$true)][string]$BuildDir)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
Import-Module (Join-Path $PSScriptRoot '../src/updater/UpdateEngine.psm1') -Force
$out=Resolve-DxlOutput $OutDir
if (Test-Path -LiteralPath $out) {throw 'Choose a new fixture directory'}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& $env:ComSpec /d /c ('"'+$vcvars+'" >nul && set') | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}
}
$target=Join-Path $out 'Installed DXL'; $job=Join-Path $out 'cache/jobs/install'; $cache=Join-Path $out 'cache'
$null=New-Item -ItemType Directory -Path $target,(Join-Path $job 'updater') -Force
$exe=Join-Path $target 'DXL.exe'
& cl.exe /nologo /utf-8 /std:c++20 /EHsc /MT /DUNICODE /D_UNICODE ('/Fo'+(Join-Path $out 'fixture.obj')) ('/Fe'+$exe) (Join-Path $SourceRoot 'tests/update_handoff.cpp') (Join-Path $BuildDir 'obj/launcher/app.res') /link /SUBSYSTEM:WINDOWS user32.lib
if ($LASTEXITCODE) {throw 'Fixture compile failed'}
& (Join-Path $PSScriptRoot 'test_updates.ps1') -OutDir (Join-Path $out 'cases') -BuiltExe $exe
$r=[pscustomobject]@{version='0.2';tag='v0.2';name='DXL-v0.2-win64.zip';url='https://github.com/LCPD15/DXL/releases/download/v0.2/DXL-v0.2-win64.zip';body='Fixture release'}
$archive=Join-Path $cache $r.name
Copy-Item -LiteralPath (Join-Path $out 'cases/good.zip') -Destination $archive
$r | Add-Member size (Get-Item -LiteralPath $archive).Length
$r | Add-Member sha256 (Get-FileHash -LiteralPath $archive).Hash
Write-DxlJson (Join-Path $cache 'available.json') $r
Write-DxlJson ($archive+'.json') $r
Copy-Item -LiteralPath (Join-Path $BuildDir 'DXL-update.exe') -Destination $job
foreach ($name in @('Update.ps1','UpdateEngine.psm1')) {Copy-Item -LiteralPath (Join-Path $SourceRoot ('src/updater/'+$name)) -Destination (Join-Path $job ('updater/'+$name))}
$parent=Start-Process -FilePath $exe -WorkingDirectory $target -WindowStyle Hidden -PassThru
$request=Join-Path $job 'request.json'
Write-DxlJson $request @{action='install';current='0.1';expected='0.2';cache=$cache;install=$target;parentPid=$parent.Id;lang='en'}
$updater=Start-Process -FilePath (Join-Path $job 'DXL-update.exe') -ArgumentList ('"'+$request+'"') -WorkingDirectory $job -WindowStyle Hidden -PassThru
try {
    Start-Sleep -Milliseconds 1000
    if ($updater.HasExited) {throw 'Updater exited before parent handoff'}
    [IO.File]::WriteAllText((Join-Path $target 'exit.flag'),'exit normally')
    if (!$parent.WaitForExit(10000) -or !$updater.WaitForExit(30000)) {throw 'Updater handoff timeout'}
    $result=Read-DxlJson (Join-Path $job 'result.json')
    if ($result.state -ne 'installed') {throw ('Install failed: '+$result.detail)}
    for ($i=0;$i -lt 40;$i++) {
        if (@(Get-Content -LiteralPath (Join-Path $target 'starts.txt')).Count -ge 2) {break}
        Start-Sleep -Milliseconds 100
    }
    if (@(Get-Content -LiteralPath (Join-Path $target 'starts.txt')).Count -ne 2) {throw 'Updated application did not restart exactly once'}
    if (Test-Path -LiteralPath $archive) {throw 'Installed archive was not removed'}
    Write-Output 'PASS standalone updater: waits for parent, replaces files, restarts application once, deletes installed archive; paths with spaces supported'
} finally {
    [IO.File]::WriteAllText((Join-Path $target 'exit.flag'),'exit normally')
}
