$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression

function Save-DxlDownload([string]$Url,[string]$Path,[long]$ExpectedSize) {
    $request=[Net.HttpWebRequest]::Create($Url)
    $request.UserAgent='DXL-Updater'
    $request.Timeout=30000; $request.ReadWriteTimeout=30000
    $response=$null; $downloadStream=$null; $output=$null
    try {
        $response=$request.GetResponse()
        if ($response.ResponseUri.Scheme -ne 'https') { throw 'Insecure download redirect' }
        $downloadStream=$response.GetResponseStream()
        $output=[IO.File]::Open($Path,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::None)
        $buffer=New-Object byte[] 1048576
        [long]$total=0
        $timer=[Diagnostics.Stopwatch]::StartNew()
        while (($count=$downloadStream.Read($buffer,0,$buffer.Length)) -gt 0) {
            $total+=$count
            if ($total -gt $ExpectedSize -or $timer.Elapsed.TotalMinutes -gt 30) { throw 'Download exceeds expected size or time limit' }
            $output.Write($buffer,0,$count)
        }
        if ($total -ne $ExpectedSize) { throw 'Incomplete download' }
    } finally {
        if ($output) {$output.Dispose()}; if ($downloadStream) {$downloadStream.Dispose()}; if ($response) {$response.Dispose()}
    }
}

function Get-DxlVersion([string]$Text) {
    if ($Text -notmatch '^v?(0|[1-9]\d*)\.(0|[1-9]\d*)(?:\.(0|[1-9]\d*))?(?:\.(0|[1-9]\d*))?$') { throw 'Invalid stable version' }
    $v = $Text.TrimStart('v').Split('.')
    while ($v.Count -lt 4) { $v += '0' }
    return [version]($v -join '.')
}
function Write-DxlJson([string]$Path, $Value) {
    $tmp = $Path + '.tmp'
    [IO.File]::WriteAllText($tmp, ($Value | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $tmp -Destination $Path -Force
}
function Read-DxlJson([string]$Path) {
    if ((Get-Item -LiteralPath $Path).Length -gt 2MB) { throw 'Metadata too large' }
    return ([IO.File]::ReadAllText($Path) | ConvertFrom-Json)
}
function Assert-DxlPath([string]$Root, [string]$Relative) {
    if (!$Relative -or $Relative -match '(^[/\\]|[:\x00-\x1f]|(^|[/\\])\.\.?([/\\]|$)|[. ]($|[/\\]))') { throw 'Unsafe package path' }
    if ($Relative -match '(^|[/\\])(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])([.]|[/\\]|$)') { throw 'Reserved device path' }
    $base = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    $full = [IO.Path]::GetFullPath((Join-Path $base $Relative))
    if (!$full.StartsWith($base+'\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Package path escapes root' }
    $p = $full
    while ($p -and $p.Length -ge $base.Length) {
        if (Test-Path -LiteralPath $p) {
            if ((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse points are not allowed' }
        }
        $p = Split-Path -Parent $p
    }
    return $full
}
function Assert-DxlRelease($Release) {
    $null = Get-DxlVersion $Release.version
    if ($Release.name -cne ('DXL-v'+$Release.version+'-win64.zip')) { throw 'Unexpected asset name' }
    if ($Release.sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Missing SHA256' }
    if ([long]$Release.size -le 0 -or [long]$Release.size -gt 2GB) { throw 'Unexpected package size' }
    $expected = 'https://github.com/LCPD15/DXL/releases/download/'+$Release.tag+'/'+$Release.name
    if ($Release.url -cne $expected -or (Get-DxlVersion $Release.tag) -ne (Get-DxlVersion $Release.version)) { throw 'Unexpected release URL' }
}
function Get-DxlRemote {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $headers = @{'User-Agent'='DXL-Updater';Accept='application/vnd.github+json';'X-GitHub-Api-Version'='2022-11-28'}
    $r = Invoke-RestMethod -Uri 'https://api.github.com/repos/LCPD15/DXL/releases/latest' -Headers $headers -TimeoutSec 20
    if ($r.draft -or $r.prerelease) { return $null }
    $null = Get-DxlVersion $r.tag_name
    $version = $r.tag_name.TrimStart('v')
    $name = 'DXL-v'+$version+'-win64.zip'
    $assets = @($r.assets | Where-Object { $_.name -ceq $name -and $_.state -eq 'uploaded' })
    if ($assets.Count -ne 1) { return $null }
    $a = $assets[0]
    if ($a.digest -notmatch '^sha256:([a-fA-F0-9]{64})$') { return $null }
    $result = [pscustomobject]@{version=$version;tag=$r.tag_name;name=$name;url=$a.browser_download_url;size=[long]$a.size;sha256=$Matches[1];body=[string]$r.body}
    Assert-DxlRelease $result
    return $result
}
function Test-DxlCached([string]$Cache, $Release) {
    try {
        Assert-DxlRelease $Release
        $path = Assert-DxlPath $Cache $Release.name
        return ((Test-Path -LiteralPath $path -PathType Leaf) -and (Get-Item -LiteralPath $path).Length -eq [long]$Release.size -and (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq $Release.sha256)
    } catch { throw }
}
function Get-DxlLocal([string]$Cache, [string]$Current) {
    $best = $null
    foreach ($file in Get-ChildItem -LiteralPath $Cache -Filter 'DXL-v*-win64.zip.json' -File) {
        try {
            $r = Read-DxlJson $file.FullName
            if ((Get-DxlVersion $r.version) -le (Get-DxlVersion $Current)) { continue }
            if ((!$best -or (Get-DxlVersion $r.version) -gt (Get-DxlVersion $best.version)) -and (Test-DxlCached $Cache $r)) { $best=$r }
        } catch { }
    }
    return $best
}
function Expand-DxlPackage([string]$Archive, [string]$Stage, [string]$Version) {
    $null = New-Item -ItemType Directory -Path $Stage
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        if ($zip.Entries.Count -gt 4096) { throw 'Too many package entries' }
        $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        [long]$total=0
        $prefix='DXL-v'+$Version+'/'
        foreach ($e in $zip.Entries) {
            $name=$e.FullName.Replace('\','/')
            if (!$name.StartsWith($prefix,[StringComparison]::Ordinal)) { throw 'Unexpected package root' }
            $rel=$name.Substring($prefix.Length)
            if (!$rel) { continue }
            $dest=Assert-DxlPath $Stage $rel.TrimEnd('/')
            if (!$names.Add($rel.TrimEnd('/'))) { throw 'Duplicate package entry' }
            if (($e.ExternalAttributes -shr 16 -band 0xF000) -eq 0xA000) { throw 'ZIP symlinks are not allowed' }
            $total += $e.Length
            if ($total -gt 4GB -or $e.Length -gt 2GB) { throw 'Expanded package too large' }
            if ($name.EndsWith('/')) { $null=New-Item -ItemType Directory -Path $dest -Force; continue }
            $null=New-Item -ItemType Directory -Path (Split-Path -Parent $dest) -Force
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e,$dest,$false)
        }
    } finally { $zip.Dispose() }
    $manifest=Read-DxlJson (Join-Path $Stage 'PACKAGE_MANIFEST.json')
    if ((Get-DxlVersion $manifest.Version) -ne (Get-DxlVersion $Version) -or $manifest.IncludesUserSettings -ne $false) { throw 'Invalid package version or contents' }
    $listed=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($f in $manifest.Files) {
        $path=Assert-DxlPath $Stage $f.Path
        if (!$listed.Add($f.Path.Replace('\','/')) -or $f.Path -match '(^|/)(settings|launcher|window)\.json$|^(profiles|WebView2|updates)/') { throw 'Invalid package manifest path' }
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -ne [long]$f.Bytes -or (Get-FileHash -LiteralPath $path).Hash -ne $f.SHA256) { throw 'Package file integrity failure' }
    }
    foreach ($required in @('DXL.exe','DXL-core.dll','DXL-inject.exe','web/app.js','DXL-update.exe','updater/Update.ps1','updater/UpdateEngine.psm1')) {
        if (!$listed.Contains($required)) { throw 'Incomplete DXL update' }
    }
    foreach ($file in Get-ChildItem -LiteralPath $Stage -File -Recurse) {
        $rel=$file.FullName.Substring($Stage.TrimEnd('\').Length+1).Replace('\','/')
        if (!$listed.Contains($rel) -and $rel -notin @('PACKAGE_MANIFEST.json','SHA256SUMS.txt')) { throw 'Unlisted package file' }
    }
    if ((Get-DxlVersion (Get-Item -LiteralPath (Join-Path $Stage 'DXL.exe')).VersionInfo.ProductVersion) -ne (Get-DxlVersion $Version)) { throw 'Executable version mismatch' }
    return $manifest
}
function Install-DxlFiles([string]$Stage,[string]$Target,[string]$Backup,$Manifest) {
    $paths=@($Manifest.Files | ForEach-Object {$_.Path}) + @('PACKAGE_MANIFEST.json','SHA256SUMS.txt')
    # Touch only files named by the new or previous package, preserving all other files.
    $oldPath=Join-Path $Target 'PACKAGE_MANIFEST.json'
    $obsolete=@()
    if (Test-Path -LiteralPath $oldPath) {
        $old=Read-DxlJson $oldPath
        $obsolete=@($old.Files | ForEach-Object {$_.Path} | Where-Object {$_ -notin $paths})
    }
    $all=@($paths+$obsolete | Select-Object -Unique)
    $null=New-Item -ItemType Directory -Path $Backup
    $existing=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($rel in $all) {
        $dest=Assert-DxlPath $Target $rel
        if ($rel -match '(^|/)(settings|launcher|window)\.json$|^(profiles|WebView2|updates)/') { throw 'Refusing to modify user data' }
        if (Test-Path -LiteralPath $dest) {
            $stream=[IO.File]::Open($dest,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
            $stream.Dispose()
            $save=Assert-DxlPath $Backup $rel
            $null=New-Item -ItemType Directory -Path (Split-Path -Parent $save) -Force
            [IO.File]::Copy($dest,$save,$false)
            $null=$existing.Add($rel)
        }
    }
    $changed=[Collections.Generic.List[string]]::new()
    try {
        foreach ($rel in $paths) {
            $dest=Assert-DxlPath $Target $rel
            $null=New-Item -ItemType Directory -Path (Split-Path -Parent $dest) -Force
            $changed.Add($rel)
            [IO.File]::Copy((Assert-DxlPath $Stage $rel),$dest,$true)
        }
        foreach ($rel in $obsolete) {
            $changed.Add($rel)
            $dest=Assert-DxlPath $Target $rel
            if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest }
        }
    } catch {
        $failure=$_
        for ($i=$changed.Count-1;$i -ge 0;$i--) {
            $rel=$changed[$i]; $dest=Assert-DxlPath $Target $rel
            if ($existing.Contains($rel)) { [IO.File]::Copy((Assert-DxlPath $Backup $rel),$dest,$true) }
            elseif (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest }
        }
        throw $failure
    }
}
function Remove-DxlExpired([string]$Cache,[string]$Installed) {
    foreach ($f in Get-ChildItem -LiteralPath $Cache -File) {
        if ($f.Name -match '^DXL-v(\d+\.\d+(?:\.\d+){0,2})-win64\.zip(?:\.json|\.part)?$') {
            try {
                if ((Get-DxlVersion $Matches[1]) -le (Get-DxlVersion $Installed)) {
                    Remove-Item -LiteralPath (Assert-DxlPath $Cache $f.Name) -Force
                }
            } catch { }
        }
    }
}
function Invoke-DxlUpdate([string]$RequestFile) {
    $q=Read-DxlJson $RequestFile
    $cache=[IO.Path]::GetFullPath($q.cache)
    $resultPath=Join-Path (Split-Path -Parent $RequestFile) 'result.json'
    $result=@{state='none'}
    $lock=$null
    $restart=$false
    try {
        $null=New-Item -ItemType Directory -Path $cache -Force
        $null=Assert-DxlPath $cache 'update.lock'
        $lock=[IO.File]::Open((Join-Path $cache 'update.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
        $current=Get-DxlVersion $q.current
        if ($q.action -eq 'check') {
            Remove-DxlExpired $cache $q.current
            $local=Get-DxlLocal $cache $q.current
            $remote=$null
            try { $remote=Get-DxlRemote } catch { }
            $candidate=$local
            if ($remote -and (Get-DxlVersion $remote.version) -gt $current -and (!$candidate -or (Get-DxlVersion $remote.version) -gt (Get-DxlVersion $candidate.version))) { $candidate=$remote }
            if ($candidate) {
                Write-DxlJson (Join-Path $cache 'available.json') $candidate
                $result=@{state=$(if (Test-DxlCached $cache $candidate) {'downloaded'} else {'available'});version=$candidate.version;body=$candidate.body;local=$true}
            }
        } elseif ($q.action -in @('download','install')) {
            $r=Read-DxlJson (Join-Path $cache 'available.json')
            Assert-DxlRelease $r
            if ($q.expected -cne $r.version) { throw 'Available version changed; check again before confirming' }
            if ((Get-DxlVersion $r.version) -le $current) { throw 'Update is not newer' }
            $archive=Assert-DxlPath $cache $r.name
            if ($q.action -eq 'download') {
                Write-DxlJson ($archive+'.json') $r
                if (!(Test-DxlCached $cache $r)) {
                    $part=$archive+'.part'
                    try {
                        [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
                        Save-DxlDownload $r.url $part ([long]$r.size)
                        if ((Get-Item -LiteralPath $part).Length -ne [long]$r.size -or (Get-FileHash -LiteralPath $part).Hash -ne $r.sha256) { throw 'Download integrity failure' }
                        Move-Item -LiteralPath $part -Destination $archive -Force
                    } finally {
                        if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue }
                    }
                }
                Write-DxlJson ($archive+'.json') $r
                $result=@{state='downloaded';version=$r.version;body=$r.body;local=$false}
            } else {
                if (!(Test-DxlCached $cache $r)) { throw 'Cached update is missing or damaged' }
                $target=[IO.Path]::GetFullPath($q.install)
                $null=Assert-DxlPath $target 'DXL.exe'
                if (!(Test-Path -LiteralPath (Join-Path $target 'DXL.exe'))) { throw 'Invalid installation folder' }
                $parent=Get-Process -Id ([int]$q.parentPid) -ErrorAction SilentlyContinue
                if ($parent) {
                    if ($parent.Path -ine (Join-Path $target 'DXL.exe')) { throw 'Unexpected parent process' }
                }
                # Older launchers close immediately and do not understand ready.json.
                # Preserve their recovery/restart behavior if extraction fails.
                if ($q.waitForReady -ne $true) {
                    if ($parent -and !$parent.WaitForExit(30000)) { throw 'DXL did not exit' }
                    $parent=$null
                    $restart=$true
                }
                $job=Split-Path -Parent $RequestFile
                $stage=Join-Path $job 'stage'; $backup=Join-Path $job 'backup'
                $manifest=Expand-DxlPackage $archive $stage $r.version
                if ($q.waitForReady -eq $true) {
                    Write-DxlJson (Join-Path $job 'ready.json') @{state='ready';version=$r.version}
                }
                if ($parent) {
                    if (!$parent.WaitForExit(30000)) { throw 'DXL did not exit' }
                }
                $restart=$true
                Install-DxlFiles $stage $target $backup $manifest
                Start-Process -FilePath (Join-Path $target 'DXL.exe') -WorkingDirectory $target
                $restart=$false
                Remove-DxlExpired $cache $r.version
                $result=@{state='installed';version=$r.version}
                # These are exact, private job children; never delete installation or user data.
                foreach ($child in @('stage','backup')) {
                    $path=Assert-DxlPath $job $child
                    Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        } else { throw 'Unknown update action' }
    } catch {
        $result=@{state=$(if ($q.action -eq 'check') {'none'} else {'error'});detail=$_.Exception.Message}
        Write-DxlJson $resultPath $result
        # A handoff-aware launcher is still open during preflight failures and
        # receives result.json itself. Avoid a second blocking error dialog.
        if ($q.action -eq 'install' -and ($restart -or $q.waitForReady -ne $true)) {
            Add-Type -AssemblyName PresentationFramework
            $message=if ($q.lang -eq 'en') {'Update could not be installed. Close games and other DXL windows, then try again. The downloaded package has been kept.'} else {'无法安装更新。请先退出游戏和其他 DXL 窗口，再重试。已下载的更新包已保留。'}
            $null=[System.Windows.MessageBox]::Show($message,'DXL Update')
            if ($restart) { Start-Process -FilePath (Join-Path $q.install 'DXL.exe') -WorkingDirectory $q.install }
        }
    } finally {
        if ($lock) { $lock.Dispose() }
        Write-DxlJson $resultPath $result
    }
}
Export-ModuleMember -Function *-Dxl*
