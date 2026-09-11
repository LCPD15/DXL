param([Parameter(Mandatory=$true)][string]$CorePath, [switch]$CoreFirst, [string]$RunLabel='chain')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
if($RunLabel -notmatch '^[A-Za-z0-9_-]+$'){throw 'Invalid run label'}
$CorePath=(Resolve-Path -LiteralPath $CorePath).Path
$out=Resolve-DxlOutput '' ('present-chain-'+$PID)
$diagnostics=Join-Path $WorkspaceRoot 'diagnostics/rdr2-startup'
New-Item -ItemType Directory -Force -Path $out,$diagnostics | Out-Null
$fixtureName='present-chain-'+$PID+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8)+'.exe'
$exe=Join-Path $out $fixtureName
$profileRoot=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'DXL/profiles'
$fixtureProfile=Join-Path $profileRoot ($fixtureName+'.json')
$params=Join-Path $profileRoot ($fixtureName+'.params.json')
if((Test-Path -LiteralPath $fixtureProfile) -or (Test-Path -LiteralPath $params)){throw 'Fixture profile already exists'}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars) | ForEach-Object {
    if($_ -match '^([^=]+)=(.*)$'){Set-Item -LiteralPath ('Env:'+$matches[1]) -Value $matches[2]}
}
$sources=@('tests/present_hook_chain.cpp','third_party/minhook/src/hook.c','third_party/minhook/src/buffer.c',
    'third_party/minhook/src/trampoline.c','third_party/minhook/src/hde/hde64.c') | ForEach-Object {Join-Path $SourceRoot $_}
& cl.exe /nologo /O2 /MT /EHsc /std:c++20 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN `
    ('/I'+(Join-Path $SourceRoot 'third_party/minhook/include')) ("/Fo$out/") ("/Fe$exe") @sources `
    /link d3d12.lib dxgi.lib user32.lib
if($LASTEXITCODE){throw 'Present chain fixture compilation failed'}
$settings=@{srEnable=$false;dlss5Enable=$false;masterEnabled=$false;diagEavesdrop=$false;
    nrSemanticMask=$false;diagNoUiPresent=$true;hkUi=135;hkUiMods=0}
New-Item -ItemType Directory -Force -Path $profileRoot | Out-Null
$settings | ConvertTo-Json | Set-Content -LiteralPath $fixtureProfile -Encoding utf8
$stdout=Join-Path $diagnostics ($RunLabel+'.stdout.log')
$stderr=Join-Path $diagnostics ($RunLabel+'.stderr.log')
try {
    $arguments=@(('"'+$CorePath+'"'))
    if($CoreFirst){$arguments+='--core-first'}
    $process=Start-Process -FilePath $exe -WorkingDirectory $out -ArgumentList $arguments -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if(!$process.WaitForExit(45000)){
        if($process.Path -and [IO.Path]::GetFullPath($process.Path) -eq [IO.Path]::GetFullPath($exe)){$process.Kill()}
        throw 'Owned presentation fixture exceeded its time bound'
    }
    $process.Refresh()
    $coreLog=Join-Path (Split-Path -Parent $profileRoot) ('diagnostics/logs/DXL-core-'+$process.Id+'.log')
    if(Test-Path -LiteralPath $coreLog){Copy-Item -LiteralPath $coreLog -Destination (Join-Path $diagnostics ($RunLabel+'.core.log')) -Force}
    @{processId=$process.Id;core=$CorePath;coreSha256=(Get-FileHash -LiteralPath $CorePath -Algorithm SHA256).Hash;
      coreFirst=[bool]$CoreFirst;executable=$exe;exitCode=$process.ExitCode;settings=$settings} |
        ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $diagnostics ($RunLabel+'.json')) -Encoding utf8
    Get-Content -LiteralPath $stdout
    if($process.ExitCode -ne 0){throw ('Present chain fixture failed: exit='+$process.ExitCode)}
    if(!(Select-String -LiteralPath $stdout -Pattern '^PASS stable Present' -Quiet)){throw 'Native presentation success missing'}
} finally {
    foreach($path in @($fixtureProfile,$params)){
        $resolved=[IO.Path]::GetFullPath($path)
        if(![IO.Path]::GetDirectoryName($resolved).Equals([IO.Path]::GetFullPath($profileRoot),[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid fixture cleanup path'}
        if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved}
    }
}
