param([string]$OutDir = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$OutDir = Resolve-DxlOutput $OutDir 'lut-update-test'
Import-Module (Join-Path $SourceRoot 'src/updater/UpdateEngine.psm1') -Force

# Exercise the actual installer in a unique external fixture, never a user install.
$fixture = Join-Path $OutDir ([Guid]::NewGuid().ToString('N'))
$stage = Join-Path $fixture 'stage'
$target = Join-Path $fixture 'target'
$backup = Join-Path $fixture 'backup'
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'lut'),(Join-Path $target 'lut') | Out-Null
$custom = Join-Path $target 'lut/My-Custom.png'
[IO.File]::WriteAllText($custom, 'user-custom-lut-kept-byte-for-byte')
[IO.File]::WriteAllText((Join-Path $target 'lut/Neutral-16.png'), 'previous-bundled-template')
[IO.File]::WriteAllText((Join-Path $stage 'lut/Neutral-16.png'), 'new-bundled-template')
[IO.File]::WriteAllText((Join-Path $stage 'lut/README.md'), 'LUT instructions')
$manifest = [pscustomobject]@{ Files = @(
    [pscustomobject]@{Path='lut/Neutral-16.png'},
    [pscustomobject]@{Path='lut/README.md'}) }
[IO.File]::WriteAllText((Join-Path $stage 'PACKAGE_MANIFEST.json'), ($manifest | ConvertTo-Json -Depth 5))
[IO.File]::WriteAllText((Join-Path $stage 'SHA256SUMS.txt'), 'fixture')
[IO.File]::WriteAllText((Join-Path $target 'PACKAGE_MANIFEST.json'), '{"Files":[{"Path":"lut/Neutral-16.png"}]}')
$before = (Get-FileHash -LiteralPath $custom).Hash
Install-DxlFiles $stage $target $backup $manifest
if ((Get-FileHash -LiteralPath $custom).Hash -ne $before) { throw 'Update modified a custom LUT' }
if ([IO.File]::ReadAllText((Join-Path $target 'lut/Neutral-16.png')) -cne 'new-bundled-template') { throw 'Bundled template was not updated' }
if ([IO.File]::ReadAllText((Join-Path $backup 'lut/Neutral-16.png')) -cne 'previous-bundled-template') { throw 'Bundled template was not backed up' }
Write-Output 'PASS update preserves custom LUTs, upgrades bundled templates and backs up old templates'
