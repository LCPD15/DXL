param([string]$OutDir='smooth-motion-tests')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$out=Resolve-DxlOutput $OutDir
$null=New-Item -ItemType Directory -Path $out -Force
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'MSVC x64 tools not found'}
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars) | ForEach-Object {
    if($_ -match '^([^=]+)=(.*)$'){Set-Item -LiteralPath ('Env:'+$matches[1]) -Value $matches[2]}
}
$exe=Join-Path $out 'smooth_motion.exe'
& cl.exe /nologo /O2 /MT /EHsc /std:c++20 /utf-8 /W4 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN `
    (Join-Path $SourceRoot 'tests/smooth_motion.cpp') ("/Fo$out/smooth_motion.obj") /link ("/OUT:$exe") user32.lib
if($LASTEXITCODE){throw 'Smooth Motion isolated fixture compilation failed'}
$process=$null
try {
    $process=Start-Process -FilePath $exe -WorkingDirectory $out -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $out 'smooth-motion.log') -RedirectStandardError (Join-Path $out 'smooth-motion.stderr.log')
    $null=$process.Handle
    if(!$process.WaitForExit(15000)){throw 'Smooth Motion isolated fixture exceeded its time bound'}
    Get-Content -LiteralPath (Join-Path $out 'smooth-motion.log')
    if($process.ExitCode -ne 0){throw ('Smooth Motion isolated fixture failed: '+$process.ExitCode)}
} finally {
    if($process){
        if(!$process.HasExited){
            if(!$process.Path -or [IO.Path]::GetFullPath($process.Path) -ne [IO.Path]::GetFullPath($exe)){throw 'Unexpected fixture process path'}
            $process.Kill()
            if(!$process.WaitForExit(5000)){throw 'Owned Smooth Motion fixture did not exit'}
        }
        $process.Dispose()
    }
}
