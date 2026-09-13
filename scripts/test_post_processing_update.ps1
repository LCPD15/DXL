param([string]$OutDir = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$OutDir = Resolve-DxlOutput $OutDir 'post-processing-update-test'
Import-Module (Join-Path $SourceRoot 'src/updater/UpdateEngine.psm1') -Force

# The production installer only touches manifest entries. User shaders, including
# nested support files, must survive an upgrade even when bundled examples change.
$fixture = Join-Path $OutDir ([Guid]::NewGuid().ToString('N'))
$stage = Join-Path $fixture 'stage'
$target = Join-Path $fixture 'target'
$backup = Join-Path $fixture 'backup'
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'post-processing'),(Join-Path $target 'post-processing/Custom') | Out-Null
$customFiles = @('post-processing/My Film.fx','post-processing/Custom/Helper.fxh')
foreach ($relative in $customFiles) { [IO.File]::WriteAllText((Join-Path $target $relative), "player-file:$relative") }
$before = @{}
foreach ($relative in $customFiles) { $before[$relative] = (Get-FileHash -LiteralPath (Join-Path $target $relative)).Hash }
[IO.File]::WriteAllText((Join-Path $target 'post-processing/01-Sepia.fx'), 'previous-bundled-example')
[IO.File]::WriteAllText((Join-Path $target 'post-processing/Retired.fx'), 'retired-bundled-example')
[IO.File]::WriteAllText((Join-Path $stage 'post-processing/01-Sepia.fx'), 'new-bundled-example')
[IO.File]::WriteAllText((Join-Path $stage 'post-processing/README.md'), 'DXL effect authoring instructions')
$manifest = [pscustomobject]@{ Files = @(
    [pscustomobject]@{Path='post-processing/01-Sepia.fx'},
    [pscustomobject]@{Path='post-processing/README.md'}) }
[IO.File]::WriteAllText((Join-Path $stage 'PACKAGE_MANIFEST.json'), ($manifest | ConvertTo-Json -Depth 5))
[IO.File]::WriteAllText((Join-Path $stage 'SHA256SUMS.txt'), 'fixture')
[IO.File]::WriteAllText((Join-Path $target 'PACKAGE_MANIFEST.json'), '{"Files":[{"Path":"post-processing/01-Sepia.fx"},{"Path":"post-processing/Retired.fx"}]}')
Install-DxlFiles $stage $target $backup $manifest
foreach ($relative in $customFiles) {
    if ((Get-FileHash -LiteralPath (Join-Path $target $relative)).Hash -cne $before[$relative]) { throw "Update modified a player file: $relative" }
}
if ([IO.File]::ReadAllText((Join-Path $target 'post-processing/01-Sepia.fx')) -cne 'new-bundled-example') { throw 'Bundled example was not updated' }
if ([IO.File]::ReadAllText((Join-Path $backup 'post-processing/01-Sepia.fx')) -cne 'previous-bundled-example') { throw 'Previous example was not backed up' }
if (Test-Path -LiteralPath (Join-Path $target 'post-processing/Retired.fx')) { throw 'Retired bundled example was not removed' }
if ([IO.File]::ReadAllText((Join-Path $backup 'post-processing/Retired.fx')) -cne 'retired-bundled-example') { throw 'Retired example was not backed up' }
Write-Output 'PASS actual installer preserves custom FX/support files, replaces bundled examples and backs up replaced/retired examples'
