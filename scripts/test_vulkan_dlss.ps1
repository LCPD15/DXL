param(
    [string]$CorePath = '',
    [string]$OutDir = '',
    [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$RunLabel = 'repro',
    [uint32]$ReplacementDelayMs = 4500,
    [switch]$AuxDxgi,
    [switch]$Headless,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$OutDir = Resolve-DxlOutput $OutDir 'vulkan-dlss-probe'
$diagnostics = Join-Path $WorkspaceRoot 'diagnostics/vulkan-dlss'
New-Item -ItemType Directory -Force -Path $OutDir,$diagnostics | Out-Null
if ($CorePath) {
    $CorePath = (Resolve-Path -LiteralPath $CorePath).Path
    $nrRuntime = Join-Path (Split-Path -Parent $CorePath) 'ngx/nvngx_dlssnr.dll'
    if (!(Test-Path -LiteralPath $nrRuntime)) { throw "DXL runtime missing: $nrRuntime" }
}
$dlssRoot = Join-Path $DependencyRoot 'dlss'
$headersRoot = Join-Path $DependencyRoot 'Vulkan-Headers'
$volkRoot = Join-Path $DependencyRoot 'volk'
foreach ($required in @(
    (Join-Path $dlssRoot 'include/nvsdk_ngx_helpers_vk.h'),
    (Join-Path $headersRoot 'include/vulkan/vulkan.h'),
    (Join-Path $volkRoot 'volk.h'),
    (Join-Path $dlssRoot 'lib/Windows_x86_64/rel/nvngx_dlss.dll'))) {
    if (!(Test-Path -LiteralPath $required)) {
        throw "Missing external dependency: $required. See DEPENDENCIES.md (optional Vulkan fixture)."
    }
}
$exe = Join-Path $OutDir 'vulkan-dlss-probe.exe'
if (!$SkipBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'MSVC x64 tools not found' }
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    & cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars) | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -LiteralPath ('Env:'+$matches[1]) -Value $matches[2] }
    }
    $compiler = Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64/cl.exe'
    & $compiler /nologo /std:c++17 /EHsc /MT /O2 /utf-8 `
        ('/I'+(Join-Path $SourceRoot 'src/common')) ('/I'+(Join-Path $headersRoot 'include')) `
        ('/I'+$volkRoot) ('/I'+(Join-Path $dlssRoot 'include')) `
        (Join-Path $SourceRoot 'tests/vulkan_dlss_probe.cpp') "/Fo$OutDir/vulkan_dlss_probe.obj" "/Fe$exe" `
        /link (Join-Path $dlssRoot 'lib/Windows_x86_64/x64/nvsdk_ngx_s.lib') advapi32.lib user32.lib shell32.lib d3d11.lib dxgi.lib
    if ($LASTEXITCODE) { throw 'Vulkan DLSS fixture compilation failed' }
}
Copy-Item -LiteralPath (Join-Path $dlssRoot 'lib/Windows_x86_64/rel/nvngx_dlss.dll') -Destination (Join-Path $OutDir 'nvngx_dlss.dll') -Force
$probeArgs = @()
if (!$Headless) { $probeArgs += @('--present','--reinit-dlss','--replacement-delay-ms',[string]$ReplacementDelayMs) }
if ($AuxDxgi) { $probeArgs += '--aux-dxgi' }
if ($CorePath) { $probeArgs += @('--dxl',$CorePath) }
$logPath = Join-Path $diagnostics ($RunLabel+'.log')
$runExe = $exe
$ownedProfilePaths = @()
$profileRoot = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'DXL/profiles'
$pushedLocation = $false
try {
if ($CorePath) {
    $fixtureName = 'vulkan-dlss-' + $PID + '-' + [Guid]::NewGuid().ToString('N').Substring(0,12) + '.exe'
    $runExe = Join-Path $OutDir $fixtureName
    Copy-Item -LiteralPath $exe -Destination $runExe
    New-Item -ItemType Directory -Force -Path $profileRoot | Out-Null
    $candidateProfilePaths = @((Join-Path $profileRoot ($fixtureName+'.json')), (Join-Path $profileRoot ($fixtureName+'.params.json')))
    foreach ($path in $candidateProfilePaths) { if (Test-Path -LiteralPath $path) { throw "Unique fixture profile already exists: $path" } }
    $ownedProfilePaths = $candidateProfilePaths
    $fixtureSettings = [ordered]@{
        srEnable=$false; dlss5Enable=$true; masterEnabled=$true; diagEavesdrop=$true
        dlss5AtEvaluate=$true; nrAutoRoute=$true; nrSemanticMask=$false
        nrOpticalFlow=$true; nrOpticalFlowQuality=1; nrRenderScale=1
        nrSelfLayers=1; nrTrueLayers=1; nrColourStrength=1; nrPreset=0; nrStyle=2
        nrIntensity=1; nrLocalTone=1; nrLocalStructure=1; nrSkinStructure=0.6
        diagNoUiPresent=$false; hkUi=135; hkUiMods=0
    }
    $fixtureSettings | ConvertTo-Json | Set-Content -LiteralPath $ownedProfilePaths[0] -Encoding utf8
}
$metadata = [ordered]@{
    utc = [DateTime]::UtcNow.ToString('o'); executable = $runExe
    executableSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $runExe).Hash
    core = $CorePath; arguments = $probeArgs
}
if ($CorePath) {
    $metadata.coreSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $CorePath).Hash
    $metadata.fixtureSettings = $fixtureSettings
}
Push-Location -LiteralPath $OutDir
$pushedLocation = $true
& $runExe @probeArgs *> $logPath
$probeExit = $LASTEXITCODE
} finally {
    if ($pushedLocation) { Pop-Location }
    foreach ($path in $ownedProfilePaths) {
        $resolved = [IO.Path]::GetFullPath($path)
        if (![IO.Path]::GetDirectoryName($resolved).Equals([IO.Path]::GetFullPath($profileRoot),[StringComparison]::OrdinalIgnoreCase)) {
            throw 'Fixture profile cleanup escaped the profiles directory'
        }
        if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved }
    }
}
$metadata.exitCode = $probeExit
$logLines = @(Get-Content -LiteralPath $logPath)
if ($logLines.Count -and $logLines[0] -match '^Vulkan DLSS probe pid=(\d+)$') {
    $metadata.processId = [uint32]$matches[1]
    $coreLog = Join-Path (Split-Path -Parent $profileRoot) ('diagnostics/logs/DXL-core-'+$metadata.processId+'.log')
    if ($CorePath -and (Test-Path -LiteralPath $coreLog)) {
        $metadata.coreLog = Join-Path $diagnostics ($RunLabel+'.core.log')
        Copy-Item -LiteralPath $coreLog -Destination $metadata.coreLog -Force
    }
}
$metadata | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $diagnostics ($RunLabel+'.json')) -Encoding utf8
$logLines | Where-Object { $_ -match '^(Vulkan|DXL |AUX |PHASE|Replacement|READBACK|PASS |FAIL )' }
Write-Output "Probe exit=$probeExit; log=$logPath"
if ($probeExit -ne 0) { throw "Vulkan DLSS regression failed (exit $probeExit). Exit 11 means DXL counters stopped after window replacement." }
