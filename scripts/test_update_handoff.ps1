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
$clientExe=Join-Path $out 'test-update-client.exe'
& cl.exe /nologo /utf-8 /std:c++20 /EHsc /MT /DUNICODE /D_UNICODE /DDXL_TEST_UPDATE_CLIENT ('/Fo'+(Join-Path $out 'client-fixture.obj')) ('/Fe'+$clientExe) (Join-Path $SourceRoot 'tests/update_handoff.cpp') /link /SUBSYSTEM:CONSOLE user32.lib shell32.lib
if ($LASTEXITCODE) {throw 'Client fixture compile failed'}
& $clientExe (Join-Path $out 'client handoff 测试')
if ($LASTEXITCODE) {throw 'Client handoff failed'}
$version=(Get-Item -LiteralPath $exe).VersionInfo.ProductVersion
& (Join-Path $PSScriptRoot 'test_updates.ps1') -OutDir (Join-Path $out 'cases') -BuiltExe $exe
$r=[pscustomobject]@{version=$version;tag=('v'+$version);name=('DXL-v'+$version+'-win64.zip');url=('https://github.com/LCPD15/DXL/releases/download/v'+$version+'/DXL-v'+$version+'-win64.zip');body='Fixture release'}
$archive=Join-Path $cache $r.name
Copy-Item -LiteralPath (Join-Path $out 'cases/good.zip') -Destination $archive
$r | Add-Member size (Get-Item -LiteralPath $archive).Length
$r | Add-Member sha256 (Get-FileHash -LiteralPath $archive).Hash
Write-DxlJson (Join-Path $cache 'available.json') $r
Write-DxlJson ($archive+'.json') $r
Copy-Item -LiteralPath (Join-Path $BuildDir 'DXL-update.exe') -Destination $job
foreach ($name in @('Update.ps1','UpdateEngine.psm1')) {Copy-Item -LiteralPath (Join-Path $SourceRoot ('src/updater/'+$name)) -Destination (Join-Path $job ('updater/'+$name))}
$parent=Start-Process -FilePath $exe -WorkingDirectory $target -WindowStyle Hidden -PassThru
$invalidCache=Join-Path $out 'invalid-cache'
$invalidJob=Join-Path $invalidCache 'job'
$null=New-Item -ItemType Directory -Path $invalidJob -Force
$invalidArchive=Join-Path $invalidCache $r.name
[IO.File]::WriteAllText($invalidArchive,'Not a ZIP despite a matching download checksum')
$invalidRelease=[pscustomobject]@{version=$r.version;tag=$r.tag;name=$r.name;url=$r.url;body='Invalid package fixture';size=(Get-Item -LiteralPath $invalidArchive).Length;sha256=(Get-FileHash -LiteralPath $invalidArchive).Hash}
Write-DxlJson (Join-Path $invalidCache 'available.json') $invalidRelease
$invalidRequest=Join-Path $invalidJob 'request.json'
Write-DxlJson $invalidRequest @{action='install';current='0.0';expected=$version;cache=$invalidCache;install=$target;parentPid=$parent.Id;lang='en';waitForReady=$true}
$request=Join-Path $job 'request.json'
Write-DxlJson $request @{action='install';current='0.0';expected=$version;cache=$cache;install=$target;parentPid=$parent.Id;lang='en';waitForReady=$true}
$updater=$null
try {
    $invalidUpdater=Start-Process -FilePath (Join-Path $job 'DXL-update.exe') -ArgumentList ('"'+$invalidRequest+'"') -WorkingDirectory $job -WindowStyle Hidden -PassThru
    if (!$invalidUpdater.WaitForExit(15000)) {throw 'Failed preflight blocked instead of returning to the launcher'}
    if ((Read-DxlJson (Join-Path $invalidJob 'result.json')).state -ne 'error') {throw 'Invalid package was accepted'}
    if (Test-Path -LiteralPath (Join-Path $invalidJob 'ready.json')) {throw 'Invalid package requested launcher shutdown'}
    if ($parent.HasExited) {throw 'Preflight failure closed the launcher'}
    Write-Output 'PASS standalone updater preflight failure: invalid ZIP returns error, emits no shutdown signal, leaves parent running and does not block on a dialog'
    $updater=Start-Process -FilePath (Join-Path $job 'DXL-update.exe') -ArgumentList ('"'+$request+'"') -WorkingDirectory $job -WindowStyle Hidden -PassThru
    for ($i=0;$i -lt 100;$i++) {
        if (Test-Path -LiteralPath (Join-Path $job 'ready.json')) {break}
        if ($updater.HasExited) {throw 'Updater failed during preflight'}
        Start-Sleep -Milliseconds 100
    }
    if (!(Test-Path -LiteralPath (Join-Path $job 'ready.json'))) {throw 'Updater did not complete package preflight'}
    if ($updater.HasExited) {throw 'Updater exited before parent handoff'}
    if ($parent.HasExited) {throw 'Parent exited before ready handoff'}
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
    Write-Output 'PASS standalone updater: validates/extracts before signaling ready, waits for parent, replaces files, restarts application once, deletes installed archive; paths with spaces supported'
} finally {
    [IO.File]::WriteAllText((Join-Path $target 'exit.flag'),'exit normally')
    # Fixtures are the only application processes this script creates.
    $null=$parent.WaitForExit(10000)
    if ($updater -and !$updater.HasExited) { $null=$updater.WaitForExit(30000) }
    for ($i=0;$i -lt 50;$i++) {
        $remaining=@(Get-Process -Name DXL -ErrorAction SilentlyContinue | Where-Object {$_.Path -ieq $exe})
        if (!$remaining.Count) {break}
        Start-Sleep -Milliseconds 100
    }
    if ($remaining.Count) {throw 'Test launcher fixture did not exit'}
}
