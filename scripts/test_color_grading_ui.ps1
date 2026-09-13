param([string]$OutDir='color-grading-ui-tests')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$out=Resolve-DxlOutput $OutDir
New-Item -ItemType Directory -Force -Path $out,"$out/obj","$out/lut" | Out-Null
Copy-Item -LiteralPath (Join-Path $SourceRoot 'lut/Neutral-16.png') -Destination "$out/lut/Neutral-16.png" -Force
if (Test-Path -LiteralPath (Join-Path $SourceRoot 'post-processing')) {
    New-Item -ItemType Directory -Force -Path "$out/post-processing" | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $SourceRoot 'post-processing') -Filter '*.fx' -File | Copy-Item -Destination "$out/post-processing" -Force
}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'MSVC x64 tools not found'}
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars) | ForEach-Object {
    if($_ -match '^([^=]+)=(.*)$'){Set-Item -LiteralPath ('Env:'+$matches[1]) -Value $matches[2]}
}
Set-Location -LiteralPath $SourceRoot
$flags=@('/nologo','/O2','/MT','/EHsc','/std:c++20','/utf-8','/W3','/DUNICODE','/D_UNICODE','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/Ithird_party/imgui','/Ithird_party/imgui/backends','/Ithird_party/minhook/include',"/Fo$out/obj/")
& cl.exe @flags tests/color_grading_settings.cpp /link "/OUT:$out/color_grading_settings.exe" shell32.lib ole32.lib user32.lib advapi32.lib
if($LASTEXITCODE){throw 'Grading settings fixture compilation failed'}
& "$out/color_grading_settings.exe"
if($LASTEXITCODE){throw 'Grading settings regression failed'}
$sources=@('src/core/ColorGrading.cpp','third_party/imgui/imgui.cpp','third_party/imgui/imgui_draw.cpp','third_party/imgui/imgui_tables.cpp','third_party/imgui/imgui_widgets.cpp','third_party/imgui/backends/imgui_impl_dx12.cpp','third_party/imgui/backends/imgui_impl_dx11.cpp','third_party/minhook/src/hook.c','third_party/minhook/src/buffer.c','third_party/minhook/src/trampoline.c','third_party/minhook/src/hde/hde64.c')
& cl.exe @flags tests/color_grading_ui.cpp @sources /link "/OUT:$out/color_grading_ui.exe" d3d11.lib d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib windowscodecs.lib user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib
if($LASTEXITCODE){throw 'Grading UI fixture compilation failed'}
$process=Start-Process -FilePath "$out/color_grading_ui.exe" -WorkingDirectory $out -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out/ui.log" -RedirectStandardError "$out/ui-error.log"
try {
    $null=$process.Handle
    if(!$process.WaitForExit(60000)){throw 'Grading UI fixture exceeded timeout'}
    Get-Content -LiteralPath "$out/ui.log"
    if($process.ExitCode){Get-Content -LiteralPath "$out/ui-error.log";throw 'Grading UI fixture failed'}
} finally {
    if(!$process.HasExited -and $process.Path -and [IO.Path]::GetFullPath($process.Path) -eq "$out\color_grading_ui.exe"){$process.Kill();$null=$process.WaitForExit(5000)}
    $process.Dispose()
}
