param([string]$OutDir = '', [switch]$Test)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$projectRoot = $SourceRoot
$OutDir = Resolve-DxlOutput $OutDir
$objectDir = Join-Path $OutDir 'obj/launcher'
$shellDir = Join-Path $OutDir 'shell'
$webDir = Join-Path $OutDir 'web'
New-Item -ItemType Directory -Force -Path $OutDir, $objectDir, $shellDir, $webDir | Out-Null

# Build into staging only. Never kill a launcher, game, or test process.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) { throw 'Visual Studio C++ tools were not found.' }
$vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
foreach ($line in (& $env:ComSpec /d /c ('"' + $vcvars + '" >nul && set'))) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
if ($LASTEXITCODE -ne 0) { throw 'vcvars64.bat failed.' }
function Invoke-BuildTool([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed ($LASTEXITCODE). Close any process using the staging artifact and retry." }
}

$wv2 = Join-Path $DependencyRoot 'webview2'
$flags = @('/nologo','/utf-8','/std:c++20','/EHsc','/O2','/MT','/W4','/permissive-',
    '/DUNICODE','/D_UNICODE','/DWIN32_LEAN_AND_MEAN','/DNOMINMAX')
Invoke-BuildTool 'rc.exe' @('/nologo', ('/fo' + (Join-Path $objectDir 'app.res')), (Join-Path $projectRoot 'src/ui/app.rc'))
Invoke-BuildTool 'cl.exe' ($flags + @('/I', (Join-Path $wv2 'include'),
    ('/Fo' + (Join-Path $objectDir 'main.obj')), ('/Fe' + (Join-Path $OutDir 'DXL.exe')),
    (Join-Path $projectRoot 'src/ui/main.cpp'), (Join-Path $objectDir 'app.res'),
    '/link','/SUBSYSTEM:WINDOWS',(Join-Path $wv2 'lib/WebView2LoaderStatic.lib'),
    'user32.lib','ole32.lib','oleaut32.lib','gdi32.lib','advapi32.lib'))
Invoke-BuildTool 'cl.exe' ($flags + @(('/Fo' + (Join-Path $objectDir 'inject.obj')),
    ('/Fe' + (Join-Path $OutDir 'DXL-inject.exe')),
    (Join-Path $projectRoot 'src/inject/inject.cpp'),'/link','/SUBSYSTEM:CONSOLE','user32.lib','psapi.lib','shell32.lib','ole32.lib'))

$shellNames = @('d3d12','d3d11','xinput1_4','dxgi')
for ($i = 0; $i -lt $shellNames.Count; ++$i) {
    $name = $shellNames[$i]
    $cppObject = Join-Path $objectDir ($name + '_cpp.obj')
    $asmObject = Join-Path $objectDir ($name + '_asm.obj')
    $shellFlags = $flags | Where-Object { $_ -notin @('/DWIN32_LEAN_AND_MEAN','/DNOMINMAX') }
    Invoke-BuildTool 'cl.exe' ($shellFlags + @(('/DSHELL_ID=' + ($i + 1)),('/Fo' + $cppObject),
        '/c',(Join-Path $projectRoot 'src/shell/proxy_shell.cpp')))
    Invoke-BuildTool 'ml64.exe' @('/nologo','/c',('/Fo' + $asmObject),
        (Join-Path $projectRoot ('src/shell/shell_' + $name + '.asm')))
    Invoke-BuildTool 'link.exe' @('/nologo','/DLL',
        ('/DEF:' + (Join-Path $projectRoot ('src/shell/shell_' + $name + '.def'))),
        ('/OUT:' + (Join-Path $shellDir ($name + '.dll'))),$cppObject,$asmObject)
}

Copy-Item -LiteralPath (Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src/ui/web') -File).FullName -Destination $webDir -Force
if ($Test) {
    $windowTest = Join-Path $objectDir 'window_state.exe'
    Invoke-BuildTool 'cl.exe' ($flags + @(('/Fo' + (Join-Path $objectDir 'window_state.obj')),('/Fe' + $windowTest),(Join-Path $projectRoot 'tests/window_state.cpp'),'/link','user32.lib'))
    Invoke-BuildTool $windowTest @()
    $migration = Join-Path $objectDir 'data_paths.exe'
    Invoke-BuildTool 'cl.exe' ($flags + @(('/Fo' + (Join-Path $objectDir 'data_paths.obj')),('/Fe' + $migration),(Join-Path $projectRoot 'tests/data_paths.cpp'),'/link','shell32.lib','ole32.lib'))
    Push-Location $objectDir
    try { Invoke-BuildTool $migration @() } finally { Pop-Location }
    $testExe = Join-Path $objectDir 'test-launcher-preferences.exe'
    Invoke-BuildTool 'cl.exe' ($flags + @(('/Fo' + (Join-Path $objectDir 'test-launcher-preferences.obj')),
        ('/Fe' + $testExe),(Join-Path $projectRoot 'tests/test-launcher-preferences.cpp')))
    Invoke-BuildTool $testExe @()
    $controlsTest = Join-Path $objectDir 'test-launcher-controls.exe'
    Invoke-BuildTool 'cl.exe' ($flags + @(('/Fo' + (Join-Path $objectDir 'test-launcher-controls.obj')),
        ('/Fe' + $controlsTest),(Join-Path $projectRoot 'tests/test-launcher-controls.cpp')))
    Invoke-BuildTool $controlsTest @()
    $mainExeTest = Join-Path $objectDir 'game_main_exe.exe'
    Invoke-BuildTool 'cl.exe' ($flags + @(('/Fo' + (Join-Path $objectDir 'game_main_exe.obj')),
        ('/Fe' + $mainExeTest),(Join-Path $projectRoot 'tests/game_main_exe.cpp'),'/link','shell32.lib','ole32.lib','advapi32.lib'))
    Invoke-BuildTool $mainExeTest @((Join-Path $objectDir 'game-main-exe-fixtures'))
    foreach ($testScript in @('test-dxl-launcher.js','test-status-render.js','test-ui-layout.js')) {
        Invoke-BuildTool 'node.exe' @((Join-Path $PSScriptRoot $testScript))
    }
}
Write-Output "PASS DXL launcher, injector, proxy shells and web assets: $OutDir"
