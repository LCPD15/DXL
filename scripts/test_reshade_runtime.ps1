param([string]$OutDir='reshade-runtime-tests',[string]$RuntimeDll='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$out=Resolve-DxlOutput $OutDir
$null=New-Item -ItemType Directory -Path $out -Force
if(!$RuntimeDll){throw 'Pass -RuntimeDll with the private DXL-ReShade.dll built by build_reshade_runtime.ps1.'}
$RuntimeDll=[IO.Path]::GetFullPath($RuntimeDll)
if(!(Test-Path -LiteralPath $RuntimeDll)){throw 'Private ReShade runtime DLL is missing'}
$fixtureDll=Join-Path $out 'DXL-ReShade.dll'
if($RuntimeDll -ne $fixtureDll){Copy-Item -LiteralPath $RuntimeDll -Destination $fixtureDll -Force}
$RuntimeDll=$fixtureDll
$rev='6db142b4b1a05c764222e5b0bd9a644b7ccfe1dc'
$downloads=Join-Path $out 'upstream-fixtures'
$null=New-Item -ItemType Directory -Path $downloads -Force
$pinnedHashes=@{
    'Daltonize.fx'='AB6006E886DB0F1FDB9845B892DEBE774EDA9EC498182373AA6ADE601D258D3F'
    'LUT.fx'='E3179C023A599D1EBE0C73FA00084A64D0BE66F4E5B43A45C1BB25C3C9DF0363'
    'ReShade.fxh'='6DABFBBAF968C3871905D2EA17F96572FF7B1CEC01310B5D0E5252B66B30174F'
    'ReShadeUI.fxh'='78ADF672DF47460297EB9FE6DD238D2AAFA24510B52B84FEB1A745DFF70EB901'
}
foreach($name in @('Daltonize.fx','LUT.fx','ReShade.fxh','ReShadeUI.fxh')) {
    $file=Join-Path $downloads $name
    if(!(Test-Path -LiteralPath $file)){Invoke-WebRequest -Uri ('https://raw.githubusercontent.com/crosire/reshade-shaders/'+$rev+'/Shaders/'+$name) -OutFile $file}
    if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $pinnedHashes[$name]){throw ('Upstream fixture hash mismatch: '+$name)}
}
@{repository='https://github.com/crosire/reshade-shaders';commit=$rev;scope='Unmodified upstream shader fixtures, outside source and never bundled';files=@($pinnedHashes.Keys | Sort-Object | ForEach-Object { @{name=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $downloads $_) -Algorithm SHA256).Hash} })} | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $downloads 'provenance.json')
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'MSVC x64 tools not found'}
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars) | ForEach-Object {
    if($_ -match '^([^=]+)=(.*)$'){Set-Item -LiteralPath ('Env:'+$matches[1]) -Value $matches[2]}
}
$exe=Join-Path $out 'reshade_runtime.exe'
& cl.exe /nologo /O2 /MT /EHsc /std:c++20 /utf-8 /W4 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN `
    /DDXL_TEST_RESHADE_PATHS `
    ('/I'+(Join-Path $SourceRoot 'third_party/imgui')) (Join-Path $SourceRoot 'tests/reshade_runtime.cpp') `
    (Join-Path $SourceRoot 'src/core/ReShadeBridge.cpp') `
    (Join-Path $SourceRoot 'third_party/imgui/imgui.cpp') (Join-Path $SourceRoot 'third_party/imgui/imgui_draw.cpp') `
    (Join-Path $SourceRoot 'third_party/imgui/imgui_tables.cpp') (Join-Path $SourceRoot 'third_party/imgui/imgui_widgets.cpp') `
    ("/Fo$out/") /link ("/OUT:$exe") d3d11.lib d3d12.lib dxgi.lib dxguid.lib user32.lib windowscodecs.lib ole32.lib shell32.lib
if($LASTEXITCODE){throw 'ReShade GPU fixture compilation failed'}
foreach($api in @('dx12','dx11')) {
    $dir=Join-Path $out $api
    $shaderDir=Join-Path $dir 'Shaders'
    $null=New-Item -ItemType Directory -Path $shaderDir -Force
    foreach($name in @('Daltonize.fx','LUT.fx','ReShade.fxh','ReShadeUI.fxh')) {
        Copy-Item -LiteralPath (Join-Path $downloads $name) -Destination (Join-Path $shaderDir $name) -Force
    }
    # Also exercise the packaged, independently authored UI compatibility macros.
    Copy-Item -LiteralPath (Join-Path $SourceRoot 'post-processing/ReShadeUI.fxh') -Destination (Join-Path $shaderDir 'ReShadeUI.fxh') -Force
    Copy-Item -LiteralPath (Join-Path $SourceRoot 'tests/fixtures/reshade/90-DXL-UI-Validation.fx') -Destination $shaderDir -Force
    $process=$null
    try {
        $process=Start-Process -FilePath $exe -ArgumentList @(('"'+$RuntimeDll+'"'),$api) -WorkingDirectory $dir -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $dir 'run.log') -RedirectStandardError (Join-Path $dir 'stderr.log')
        $null=$process.Handle
        if(!$process.WaitForExit(180000)){throw ($api+' ReShade GPU fixture exceeded time bound')}
        Get-Content -LiteralPath (Join-Path $dir 'run.log')
        if($process.ExitCode){throw ($api+' ReShade GPU fixture failed: '+$process.ExitCode)}
        foreach($name in @('Daltonize.fx','LUT.fx','ReShade.fxh')) {
            if((Get-FileHash -LiteralPath (Join-Path $shaderDir $name) -Algorithm SHA256).Hash -ne $pinnedHashes[$name]){throw ('Fixture changed upstream source: '+$name)}
        }
    } finally {
        if($process){
            if(!$process.HasExited){
                if([IO.Path]::GetFullPath($process.Path) -ne $exe){throw 'Unexpected fixture process path'}
                $process.Kill()
                if(!$process.WaitForExit(5000)){throw 'Owned fixture did not exit'}
            }
            $process.Dispose()
        }
    }
}
@{
    result='PASS';runtimeSha256=(Get-FileHash -LiteralPath $RuntimeDll -Algorithm SHA256).Hash
    backends=@('D3D12 WARP real swapchain','D3D11 WARP real swapchain','D3D11 private texture bridge')
    coverage=@('upstream byte-exact shader output','include and external PNG','multipass intermediate texture','native uniform and UI reset','preset isolation and disabled uniform persistence','cold-start save preserves preset','source enabled annotation defaults off','stale staging reconciliation','master bypass','resize and COM lifetime','native state preservation','D3D validation')
    upstreamRevision=$rev;upstreamSourceUnmodified=$true;ownedTestProcessesRemaining=0
} | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $out 'verification.json')
