param()
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
New-Item -ItemType Directory -Force -Path $DependencyRoot,(Join-Path $WorkspaceRoot 'downloads')|Out-Null
$dlss=Join-Path $DependencyRoot 'dlss'
$revision='a291cc7d2cc642a51566f3dfd5376f635cd1b284'
if (!(Test-Path -LiteralPath (Join-Path $dlss 'include/nvsdk_ngx.h'))) {
    if ((Test-Path -LiteralPath $dlss) -and @(Get-ChildItem -LiteralPath $dlss -Force).Count) { throw 'DLSS destination is not empty; inspect it manually.' }
    git init $dlss
    if ($LASTEXITCODE) { throw 'git init failed' }
    git -C $dlss remote add origin https://github.com/NVIDIA/DLSS.git
    git -C $dlss config core.sparseCheckout true
    [IO.File]::WriteAllText((Join-Path $dlss '.git/info/sparse-checkout'),"include/\nlib/Windows_x86_64/x64/\nlib/Windows_x86_64/rel/\nLICENSE.txt\n".Replace('\n',"`n"))
    git -C $dlss fetch --depth 1 origin $revision
    if ($LASTEXITCODE) { throw 'DLSS fetch failed' }
    git -C $dlss checkout --detach FETCH_HEAD
    if ($LASTEXITCODE) { throw 'DLSS checkout failed' }
}
$webview=Join-Path $DependencyRoot 'webview2'
if (!(Test-Path -LiteralPath (Join-Path $webview 'include/WebView2.h'))) {
    $version='1.0.3485.44'
    $archive=Join-Path $WorkspaceRoot "downloads/webview2-$version.zip"
    Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$version/microsoft.web.webview2.$version.nupkg" -OutFile $archive
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip=[IO.Compression.ZipFile]::OpenRead($archive)
    try {
        foreach ($entry in $zip.Entries) {
            $relative=$null
            if ($entry.FullName -like 'build/native/include/*.h') { $relative='include/'+$entry.Name }
            elseif ($entry.FullName -eq 'build/native/x64/WebView2LoaderStatic.lib') { $relative='lib/'+$entry.Name }
            elseif ($entry.Name -match '(?i)license|notice') { $relative='licenses/'+$entry.Name }
            if ($relative -and $entry.Name) {
                $dest=Join-Path $webview $relative
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest)|Out-Null
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$dest,$true)
            }
        }
    } finally { $zip.Dispose() }
}
Write-Output 'PASS SDK inputs ready outside source. Supply runtime/model inputs as described in DEPENDENCIES.md.'
