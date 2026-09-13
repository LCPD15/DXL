param([Parameter(Mandatory=$true)][string]$SourceDir,[string]$OutDir = '')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$source=[IO.Path]::GetFullPath($SourceDir).TrimEnd('\','/')
$fileVersion=(Get-Item -LiteralPath (Join-Path $source 'DXL.exe')).VersionInfo.ProductVersion
if ($fileVersion -notmatch '^(\d+\.\d+)(?:\.(\d+))?') { throw 'Cannot determine the built application version.' }
$version=$Matches[1]
if ($Matches[2] -and $Matches[2] -ne '0') { $version+='.'+$Matches[2] }
if (!$OutDir) { $OutDir=Join-Path (Split-Path -Parent $WorkspaceRoot) ('DXL/DXL-v'+$version) }
if ((Split-Path -Leaf $OutDir) -notmatch ('(?<!\d)'+[regex]::Escape($version)+'(?!\d|\.\d)')) { $OutDir=Join-Path $OutDir ('DXL-v'+$version) }
$out=Resolve-DxlOutput $OutDir
if ($source.Equals($out,[StringComparison]::OrdinalIgnoreCase) -or $out.StartsWith($source+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Choose a separate release destination.' }
if ((Test-Path -LiteralPath $out) -and @(Get-ChildItem -LiteralPath $out -Force).Count) { throw 'Release destination must be empty.' }
$assets=@('DXL.exe','DXL-core.dll','DXL-ReShade.dll','DXL-inject.exe',
 'DXL-update.exe','updater/Update.ps1','updater/UpdateEngine.psm1',
 'shell/d3d12.dll','shell/d3d11.dll','shell/dxgi.dll','shell/xinput1_4.dll',
 'ngx/nvngx_dlssnr.dll','ngx/nvngx_dlss.dll','ngx/nvngx_dlssg.dll',
 'extensions/semantic/nvinfer_lean_11.dll','extensions/semantic/models/yolo11n-seg.plan',
 'web/index.html','web/app.js','web/updates.js','web/style.css','web/i18n.js','web/app-icon.png','web/app-icon@2x.png',
 'README_ZH.md','README_EN.md','CHANGELOG.md','THIRD_PARTY_NOTICES.md','LICENSE_STATUS.md','LICENSE','SOURCE_CODE.md','UPDATES.md',
 'DXL-Guide-ZH.docx','DXL-Guide-EN.docx','lut/Neutral-16.png','lut/README.md',
 'post-processing/01-Sepia.fx','post-processing/02-Letterbox.fx','post-processing/ReShade.fxh','post-processing/ReShadeUI.fxh','post-processing/README.md')
$assets += Get-ChildItem -LiteralPath (Join-Path $source 'licenses') -File | ForEach-Object {'licenses/'+$_.Name}
foreach ($relative in $assets) {
 $from=Join-Path $source $relative
 if (!(Test-Path -LiteralPath $from -PathType Leaf)) { throw "Missing release file: $relative" }
 $to=Join-Path $out $relative
 New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to)|Out-Null
 [IO.File]::Copy($from,$to,$false)
 if ((Get-FileHash -LiteralPath $from).Hash -ne (Get-FileHash -LiteralPath $to).Hash) { throw "Hash mismatch: $relative" }
}
$records=@(Get-ChildItem -LiteralPath $out -File -Recurse | ForEach-Object {
 [ordered]@{Path=$_.FullName.Substring($out.Length+1).Replace('\','/');Bytes=$_.Length;SHA256=(Get-FileHash -LiteralPath $_.FullName).Hash}
})
[ordered]@{Version=$version;IncludesUserSettings=$false;SemanticModel='YOLO11n-seg';LicenseInfo='LICENSE_STATUS.md';Files=$records} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $out 'PACKAGE_MANIFEST.json') -Encoding UTF8
$records | ForEach-Object {"$($_.SHA256)  $($_.Path)"} | Set-Content -LiteralPath (Join-Path $out 'SHA256SUMS.txt') -Encoding Ascii
Write-Output ("PASS complete package: {0} files, {1:N2} MiB" -f $records.Count,(($records | ForEach-Object {$_.Bytes} | Measure-Object -Sum).Sum/1MB))
