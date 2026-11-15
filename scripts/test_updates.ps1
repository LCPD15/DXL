param([Parameter(Mandatory=$true)][string]$OutDir,[Parameter(Mandatory=$true)][string]$BuiltExe)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot '../src/updater/UpdateEngine.psm1') -Force
function Assert($Value,[string]$Message) { if (!$Value) { throw $Message } }
function Reject([scriptblock]$Action,[string]$Message) {
    $rejected=$false; try { & $Action } catch { $rejected=$true }
    Assert $rejected $Message
}
$root=[IO.Path]::GetFullPath($OutDir)
$packageVersion=(Get-Item -LiteralPath $BuiltExe).VersionInfo.ProductVersion
$null=Get-DxlVersion $packageVersion
if (Test-Path -LiteralPath $root) { throw 'Use a new test output folder' }
$null=New-Item -ItemType Directory -Path $root
Assert ((Get-DxlVersion '0.10') -gt (Get-DxlVersion '0.2')) 'Numeric comparison'
Assert ((Get-DxlVersion 'v0.2') -eq (Get-DxlVersion '0.2.0')) 'Normalized version'
foreach ($version in @('0.2-beta','0.2;bad','1','01.2','v0.2/evil')) { Reject {Get-DxlVersion $version} 'Invalid version accepted' }
foreach ($path in @('../outside','..\outside','C:\outside','file:stream','folder/../outside','/outside','folder/name.','NUL','folder/COM1.txt')) { Reject {Assert-DxlPath $root $path} 'Unsafe path accepted' }
$cache=Join-Path $root 'cache'; $null=New-Item -ItemType Directory -Path $cache
$archive=Join-Path $cache 'DXL-v0.2-win64.zip'
[IO.File]::WriteAllText($archive,'test cached archive')
$release=[pscustomobject]@{version='0.2';tag='v0.2';name='DXL-v0.2-win64.zip';url='https://github.com/LCPD15/DXL/releases/download/v0.2/DXL-v0.2-win64.zip';size=(Get-Item $archive).Length;sha256=(Get-FileHash $archive).Hash;body='test notes'}
function Read-FixtureManifest([string]$Json) {
    $stream=[IO.MemoryStream]::new([Text.Encoding]::UTF8.GetBytes($Json))
    try { Read-DxlReleaseStream $stream } finally { $stream.Dispose() }
}
$release.body='Test ' + [char]0x66F4 + [char]0x65B0
$manifestJson=$release | ConvertTo-Json
$parsed=Read-FixtureManifest ([char]0xFEFF+$manifestJson)
Assert ($parsed.body -ceq $release.body) 'UTF-8 release notes changed'
Reject { Read-FixtureManifest ('['+$manifestJson+']') } 'Array manifest accepted'
Reject { Read-FixtureManifest (' ' * (2MB+1)) } 'Oversized manifest accepted'
foreach ($bad in @(
    @{key='tag';value='v0.2.0'},@{key='version';value="0.2`n"},@{key='sha256';value=('a' * 63)},
    @{key='url';value='https://example.org/DXL-v0.2-win64.zip'},@{key='name';value='DXL-v0.2-source.zip'},
    @{key='size';value=0},@{key='size';value=2.5},@{key='body';value=@('not a string')}
)) {
    $invalid=$manifestJson | ConvertFrom-Json
    $invalid.($bad.key)=$bad.value
    Reject { Read-FixtureManifest ($invalid | ConvertTo-Json) } ("Invalid manifest accepted: "+$bad.key)
}
Write-DxlJson ($archive+'.json') $release
Assert ((Get-DxlLocal $cache '0.1').version -eq '0.2') 'Cached update missing'
Assert ($null -eq (Get-DxlLocal $cache '0.2')) 'Equal version offered'
$module=Get-Module UpdateEngine
& $module { function script:Get-DxlRemote { throw 'Offline fixture' } }
$job=Join-Path $root 'check'; $null=New-Item -ItemType Directory -Path $job
$request=Join-Path $job 'request.json'
Write-DxlJson $request @{cache=$cache;current='0.1';action='check'}
Invoke-DxlUpdate $request
Assert ((Read-DxlJson (Join-Path $job 'result.json')).state -eq 'downloaded') 'Offline cache check failed'
Write-DxlJson $request @{cache=$cache;current='0.2';action='check'}
Invoke-DxlUpdate $request
$offlineResult=Read-DxlJson (Join-Path $job 'result.json')
Assert ($offlineResult.state -eq 'none' -and $offlineResult.detail -match 'Offline fixture') 'Missing offline check diagnostic'
Assert (!(Test-Path $archive)) 'Expired package retained'
[IO.File]::WriteAllText($archive,'damaged')
Assert (!(Test-DxlCached $cache $release)) 'Corrupt cache accepted'
$fixture=Join-Path $root 'fixture';$null=New-Item -ItemType Directory -Path $fixture
$files=@('DXL.exe','DXL-core.dll','DXL-inject.exe','web/app.js','DXL-update.exe','updater/Update.ps1','updater/UpdateEngine.psm1')
$records=@()
foreach ($rel in $files) {
    $p=Assert-DxlPath $fixture $rel
    $null=New-Item -ItemType Directory -Path (Split-Path -Parent $p) -Force
    if ($rel -eq 'DXL.exe') {Copy-Item -LiteralPath $BuiltExe -Destination $p} else {[IO.File]::WriteAllText($p,'fixture '+$rel)}
    $records += @{Path=$rel;Bytes=(Get-Item $p).Length;SHA256=(Get-FileHash $p).Hash}
}
$manifest=@{Version=$packageVersion;IncludesUserSettings=$false;Files=$records}
Write-DxlJson (Join-Path $fixture 'PACKAGE_MANIFEST.json') $manifest
[IO.File]::WriteAllText((Join-Path $fixture 'SHA256SUMS.txt'),'fixture')
$goodZip=Join-Path $root 'good.zip'
$zip=[IO.Compression.ZipFile]::Open($goodZip,[IO.Compression.ZipArchiveMode]::Create)
foreach ($f in Get-ChildItem $fixture -Recurse -File) {
    $rel=$f.FullName.Substring($fixture.Length+1).Replace('\','/')
    $null=[IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$f.FullName,('DXL-v'+$packageVersion+'/'+$rel))
}
$zip.Dispose()
$stage=Join-Path $root 'stage'
$m=Expand-DxlPackage $goodZip $stage $packageVersion
Assert ($m.Files.Count -eq $files.Count) 'Valid package rejected'
$badZip=Join-Path $root 'bad.zip';Copy-Item $goodZip $badZip
$zip=[IO.Compression.ZipFile]::Open($badZip,[IO.Compression.ZipArchiveMode]::Update)
$null=$zip.CreateEntry('DXL-v'+$packageVersion+'/../../escape.txt');$zip.Dispose()
Reject {Expand-DxlPackage $badZip (Join-Path $root 'bad-stage') $packageVersion} 'Zip traversal accepted'
$target=Join-Path $root 'target';$null=New-Item -ItemType Directory -Path $target
[IO.File]::WriteAllText((Join-Path $target 'DXL.exe'),'old executable')
[IO.File]::WriteAllText((Join-Path $target 'keep.txt'),'unrelated data')
$missing=Join-Path $root 'missing';$null=New-Item -ItemType Directory -Path $missing
[IO.File]::WriteAllText((Join-Path $missing 'DXL.exe'),'new executable')
$broken=@{Files=@(@{Path='DXL.exe'},@{Path='missing.dll'})}
Reject {Install-DxlFiles $missing $target (Join-Path $root 'rollback') $broken} 'Failure was not propagated'
Assert ([IO.File]::ReadAllText((Join-Path $target 'DXL.exe')) -eq 'old executable') 'Rollback did not restore executable'
$held=[IO.File]::Open((Join-Path $target 'DXL.exe'),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
try {Reject {Install-DxlFiles $stage $target (Join-Path $root 'locked') $m} 'Locked executable was accepted'} finally {$held.Dispose()}
Install-DxlFiles $stage $target (Join-Path $root 'backup') $m
Assert ((Get-FileHash (Join-Path $target 'DXL.exe')).Hash -eq (Get-FileHash $BuiltExe).Hash) 'Install failed'
Assert ([IO.File]::ReadAllText((Join-Path $target 'keep.txt')) -eq 'unrelated data') 'Unrelated file changed'
$downloadRelease=[pscustomobject]@{version='0.2';tag='v0.2';name='DXL-v0.2-win64.zip';url='https://github.com/LCPD15/DXL/releases/download/v0.2/DXL-v0.2-win64.zip';size=(Get-Item $goodZip).Length;sha256=(Get-FileHash $goodZip).Hash;body='Download fixture notes'}
& $module {
    param($release,$source)
    $script:fixtureRelease=$release; $script:fixtureSource=$source
    function script:Get-DxlRemote { return $script:fixtureRelease }
    function script:Save-DxlDownload([string]$Url,[string]$Path,[long]$ExpectedSize) {Copy-Item -LiteralPath $script:fixtureSource -Destination $Path -Force}
} $downloadRelease $goodZip
Write-DxlJson $request @{cache=$cache;current='0.2';action='check'}
Invoke-DxlUpdate $request
$equalResult=Read-DxlJson (Join-Path $job 'result.json')
Assert ($equalResult.state -eq 'none' -and !$equalResult.detail) 'Equal remote version triggered a prompt or an error'
[IO.File]::WriteAllText($archive,'test cached archive')
Write-DxlJson ($archive+'.json') $release
Write-DxlJson $request @{cache=$cache;current='0.1';action='check'}
Invoke-DxlUpdate $request
$offer=Read-DxlJson (Join-Path $job 'result.json')
Assert ($offer.state -eq 'available' -and !$offer.local -and $offer.body -eq $downloadRelease.body) 'Replaced remote ZIP incorrectly offered as cached'
Assert ((Read-DxlJson (Join-Path $cache 'available.json')).sha256 -eq $downloadRelease.sha256) 'Outdated hash retained'
Write-DxlJson $request @{cache=$cache;current='0.1';action='download';expected='0.2'}
Invoke-DxlUpdate $request
Assert ((Read-DxlJson (Join-Path $job 'result.json')).state -eq 'downloaded') 'Download did not complete'
Assert (Test-DxlCached $cache $downloadRelease) 'Downloaded package not reusable'
& $module {function script:Get-DxlRemote {throw 'Offline fixture'}}
Write-DxlJson $request @{cache=$cache;current='0.1';action='check'}
Invoke-DxlUpdate $request
Assert ((Read-DxlJson (Join-Path $job 'result.json')).state -eq 'downloaded') 'Deferred download lost on next check'
Write-DxlJson $request @{cache=$cache;current='0.1';action='download';expected='0.3'}
Invoke-DxlUpdate $request
Assert ((Read-DxlJson (Join-Path $job 'result.json')).state -eq 'error') 'Version changed after consent was accepted'
Remove-Item -LiteralPath $archive
& $module {function script:Save-DxlDownload([string]$Url,[string]$Path,[long]$ExpectedSize) {[IO.File]::WriteAllText($Path,'truncated download')}}
Write-DxlJson $request @{cache=$cache;current='0.1';action='download';expected='0.2'}
Invoke-DxlUpdate $request
Assert ((Read-DxlJson (Join-Path $job 'result.json')).state -eq 'error') 'Truncated download accepted'
Assert (!(Test-Path -LiteralPath $archive)) 'Failed download promoted to installable package'
Assert (!(Test-Path -LiteralPath ($archive+'.part'))) 'Truncated download left a partial package'
& $module {function script:Save-DxlDownload([string]$Url,[string]$Path,[long]$ExpectedSize) {[IO.File]::WriteAllText($Path,'partial network transfer');throw 'Network interrupted fixture'}}
Invoke-DxlUpdate $request
Assert ((Read-DxlJson (Join-Path $job 'result.json')).state -eq 'error') 'Network interruption was not reported'
Assert (!(Test-Path -LiteralPath ($archive+'.part'))) 'Network failure left a partial package'
Write-DxlJson $request @{cache=$cache;current='0.1';action='install';expected='0.2';waitForReady=$true;install=$target;parentPid=$PID;lang='en'}
Invoke-DxlUpdate $request
Assert ((Read-DxlJson (Join-Path $job 'result.json')).state -eq 'error') 'Missing-cache installation was not rejected'
Assert (!(Test-Path -LiteralPath (Join-Path $job 'ready.json'))) 'Failed preflight requested launcher shutdown'
Write-Output 'PASS updater: versions, offline cache, quiet no-update, expiry, corrupt ZIP, traversal, rollback, locked files, valid install and user-file preservation'
Write-Output 'PASS download lifecycle: remote offer, verified download, deferred offline install, version-consent binding and interrupted download rejection'
Write-Output 'PASS preflight: missing cache returns an error without requesting launcher shutdown; interrupted downloads remove partial files'
