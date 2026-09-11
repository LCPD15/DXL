param([Parameter(Mandatory=$true)][string]$From)
$ErrorActionPreference='Stop'
$source=(Resolve-Path -LiteralPath $From).Path
$destination=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'DXL'
if (Test-Path -LiteralPath (Join-Path $destination 'settings.json')) { throw 'DXL already has settings; no existing settings were changed.' }
New-Item -ItemType Directory -Force -Path $destination,(Join-Path $destination 'profiles')|Out-Null
$files=@()
foreach ($name in @('launcher.json','settings.json')) {
    $p=Join-Path $source $name
    if (Test-Path -LiteralPath $p -PathType Leaf) { $files+=Get-Item -LiteralPath $p }
}
$profiles=Join-Path $source 'profiles'
if (Test-Path -LiteralPath $profiles -PathType Container) { $files+=Get-ChildItem -LiteralPath $profiles -File -Filter '*.json' }
if (!$files.Count) { throw 'No configuration files found.' }
# Copy settings last, matching the native first-run importer.
foreach ($file in ($files|Sort-Object @{Expression={if($_.Name -eq 'settings.json'){1}else{0}}})) {
    if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
    $relative=$file.FullName.Substring($source.Length).TrimStart('\','/')
    $to=Join-Path $destination $relative
    if (!(Test-Path -LiteralPath $to)) { Copy-Item -LiteralPath $file.FullName -Destination $to }
}
[IO.File]::WriteAllText((Join-Path $destination 'config-migration-v1.txt'),"DXL configuration imported; source retained.")
Write-Output 'PASS configuration imported into per-user DXL; original files retained.'
