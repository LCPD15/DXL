param([string]$OutDir = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$OutDir = Resolve-DxlOutput $OutDir 'overlay-backend-tests'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC x64 tools not found' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars) | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -LiteralPath ('Env:' + $matches[1]) -Value $matches[2] }
}
$sources = @(
    'third_party/imgui/imgui.cpp', 'third_party/imgui/imgui_draw.cpp',
    'third_party/imgui/imgui_tables.cpp', 'third_party/imgui/imgui_widgets.cpp',
    'third_party/imgui/backends/imgui_impl_dx12.cpp', 'third_party/imgui/backends/imgui_impl_dx11.cpp',
    'third_party/minhook/src/hook.c', 'third_party/minhook/src/buffer.c',
    'third_party/minhook/src/trampoline.c', 'third_party/minhook/src/hde/hde64.c'
) | ForEach-Object { Join-Path $SourceRoot $_ }
foreach ($name in @('test-imgui12', 'test-imgui11')) {
    $objects = Join-Path $OutDir ($name + '-objects')
    New-Item -ItemType Directory -Force -Path $objects | Out-Null
    $exe = Join-Path $OutDir ($name + '.exe')
    $flags = @('/nologo', '/O2', '/MT', '/EHsc', '/std:c++20', '/utf-8', '/W3',
        '/DUNICODE', '/D_UNICODE', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN',
        ('/I' + (Join-Path $SourceRoot 'third_party/imgui')),
        ('/I' + (Join-Path $SourceRoot 'third_party/imgui/backends')),
        ('/I' + (Join-Path $SourceRoot 'third_party/minhook/include')), ('/Fo' + $objects + '/'))
    & cl.exe @flags (Join-Path $SourceRoot ('tests/' + $name + '.cpp')) @sources `
        /link ('/OUT:' + $exe) d3d11.lib d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib
    if ($LASTEXITCODE) { throw "$name compilation failed" }
    $process = Start-Process -FilePath $exe -WorkingDirectory $OutDir -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $OutDir ($name + '.stdout.log')) `
        -RedirectStandardError (Join-Path $OutDir ($name + '.stderr.log'))
    try {
        $null = $process.Handle
        if (!$process.WaitForExit(60000)) { throw "$name exceeded its timeout" }
        Get-Content -LiteralPath (Join-Path $OutDir ($name + '.stdout.log'))
        if ($process.ExitCode -ne 0) { throw "$name failed: $($process.ExitCode)" }
    } finally {
        if (!$process.HasExited -and $process.Path -and [IO.Path]::GetFullPath($process.Path) -eq $exe) {
            $process.Kill()
            if (!$process.WaitForExit(5000)) { throw "$name did not exit after failure cleanup" }
        }
    }
}
Write-Output 'PASS D3D12 and D3D11 overlay rendering, input, queue/format changes and resource restoration'
