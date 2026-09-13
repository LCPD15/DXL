param([string]$OutDir='color-grading-tests',[switch]$Hardware)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$out=Resolve-DxlOutput $OutDir
$null=New-Item -ItemType Directory -Path $out -Force
$fxFixture=Join-Path $out 'post-processing'
$null=New-Item -ItemType Directory -Path $fxFixture -Force
Copy-Item -LiteralPath (Join-Path $SourceRoot 'post-processing/01-Sepia.fx') -Destination (Join-Path $fxFixture 'sample-Sepia.fx') -Force
Copy-Item -LiteralPath (Join-Path $SourceRoot 'post-processing/02-Letterbox.fx') -Destination (Join-Path $fxFixture 'sample-Letterbox.fx') -Force
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'MSVC x64 tools not found'}
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars) | ForEach-Object {
    if($_ -match '^([^=]+)=(.*)$'){Set-Item -LiteralPath ('Env:'+$matches[1]) -Value $matches[2]}
}
$exe=Join-Path $out 'color_grading.exe'
& cl.exe /nologo /O2 /MT /EHsc /std:c++20 /utf-8 /W4 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN `
    (Join-Path $SourceRoot 'tests/color_grading.cpp') (Join-Path $SourceRoot 'src/core/ColorGrading.cpp') ("/Fo$out/") `
    /link ("/OUT:$exe") d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib windowscodecs.lib ole32.lib user32.lib
if($LASTEXITCODE){throw 'Color grading GPU fixture compilation failed'}
$process=$null
try{
    $adapterArgument=if($Hardware){'--hardware'}else{'--warp'}
    $process=Start-Process -FilePath $exe -ArgumentList $adapterArgument -WorkingDirectory $out -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $out 'color-grading.log') -RedirectStandardError (Join-Path $out 'color-grading.stderr.log')
    $null=$process.Handle
    if(!$process.WaitForExit(60000)){throw 'Color grading GPU fixture exceeded its time bound'}
    Get-Content -LiteralPath (Join-Path $out 'color-grading.log')
    if($process.ExitCode){throw ('Color grading GPU fixture failed: '+$process.ExitCode)}
}finally{
    if($process){
        if(!$process.HasExited){
            if(!$process.Path -or [IO.Path]::GetFullPath($process.Path) -ne [IO.Path]::GetFullPath($exe)){throw 'Unexpected fixture process path'}
            $process.Kill()
            if(!$process.WaitForExit(5000)){throw 'Owned color grading fixture did not exit'}
        }
        $process.Dispose()
    }
}
