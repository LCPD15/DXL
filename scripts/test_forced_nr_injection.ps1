param([Parameter(Mandatory=$true)][string]$CoreDir)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$runRoot=Resolve-DxlOutput '' ('forced-nr-e2e-0.7-'+$PID)
$stage=Join-Path $runRoot 'tool'
New-Item -ItemType Directory -Force -Path $stage,(Join-Path $stage 'ngx') | Out-Null
Copy-Item -LiteralPath (Join-Path $CoreDir 'DXL-core.dll') -Destination $stage
Copy-Item -LiteralPath (Join-Path $CoreDir 'ngx/nvngx_dlssnr.dll') -Destination (Join-Path $stage 'ngx')
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
& cmd.exe /d /s /c ('call "{0}" >nul && set' -f $vcvars) | ForEach-Object {
 if($_ -match '^([^=]+)=(.*)$'){Set-Item -LiteralPath ('Env:'+$matches[1]) -Value $matches[2]}
}
$fixtureName='forced-nr-'+$PID+'.exe'
$fixture=Join-Path $runRoot $fixtureName
& cl.exe /nologo /LD /MT ('/Fo'+(Join-Path $runRoot 'fg-stub.obj')) ('/Fe'+(Join-Path $stage 'nvngx_dlssg.dll')) (Join-Path $SourceRoot 'tests/fg_presence_stub.cpp') /link ('/IMPLIB:'+(Join-Path $runRoot 'fg-stub.lib'))
if($LASTEXITCODE){throw 'FG stub compilation failed'}
& cl.exe /nologo /O2 /MT /EHsc /std:c++20 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ('/Fo'+(Join-Path $runRoot 'target.obj')) ('/Fe'+$fixture) (Join-Path $SourceRoot 'tests/forced_nr_target.cpp') /link shell32.lib ole32.lib advapi32.lib
if($LASTEXITCODE){throw 'Forced target compilation failed'}
$profileRoot=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'DXL/profiles'
$profile=Join-Path $profileRoot ($fixtureName+'.json')
$params=Join-Path $profileRoot ($fixtureName+'.params.json')
if((Test-Path -LiteralPath $profile) -or (Test-Path -LiteralPath $params)){throw 'Owned fixture profile already exists'}
$ownedLogs=[Collections.Generic.List[string]]::new()
try {
 @{srEnable=$false;dlss5Enable=$true;nrProcessingEnabled=$true;masterEnabled=$true;
   nrForceCompatibility=$false;dlss5AtEvaluate=$true;nrAutoRoute=$false;diagEavesdrop=$true;
   nrRenderScale=0.5;nrOpticalFlow=$true;diagNoUiPresent=$false;overlayVk=0;overlayMods=0} |
   ConvertTo-Json | Set-Content -LiteralPath $profile -Encoding utf8
 foreach($mode in @('plain','fg','cold')) {
  @{nrForceCompatibility=($mode -eq 'cold')} | ConvertTo-Json | Set-Content -LiteralPath $params -Encoding utf8
  $stdout=Join-Path $runRoot ($mode+'.stdout.log');$stderr=Join-Path $runRoot ($mode+'.stderr.log')
  $process=$null
  try {
   $process=Start-Process -FilePath $fixture -WorkingDirectory $runRoot -ArgumentList @(('"'+(Join-Path $stage 'DXL-core.dll')+'"'),$mode) -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
   $null=$process.Handle
   if(!$process.WaitForExit(45000)){throw 'Forced NR fixture timed out'}
   $process.Refresh()
   $coreLog=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) ('DXL/diagnostics/logs/DXL-core-'+$process.Id+'.log')
   if(Test-Path -LiteralPath $coreLog){Copy-Item -LiteralPath $coreLog -Destination (Join-Path $runRoot ($mode+'.core.log'));$ownedLogs.Add($coreLog)}
   Get-Content -LiteralPath $stdout
   if($process.ExitCode){Get-Content -LiteralPath $stderr;throw ('Forced NR fixture failed: '+$mode)}
   $saved=Get-Content -LiteralPath $params -Raw | ConvertFrom-Json
   if(!$saved.nrForceCompatibility -or [Math]::Abs($saved.nrSkinStructure-0.37) -gt 0.001){throw 'In-game save did not preserve forced-mode preference'}
   if((Get-Content -LiteralPath $coreLog -Raw) -notmatch 'NR force compatibility applied:'){throw 'No final-output NR processing logged'}
   Copy-Item -LiteralPath $params -Destination (Join-Path $runRoot ($mode+'.params.json'))
  } finally {
   if($process){if(!$process.HasExited){$process.Kill();$null=$process.WaitForExit(5000)};$process.Dispose()}
  }
 }
 Write-Output ('PASS full-core force compatibility: native-route wait, final NR, off/on restoration, FG-heuristic opt-in, IPC persistence; evidence='+$runRoot)
} finally {
 foreach($path in @($profile,$params)+@($ownedLogs)) {
  if(Test-Path -LiteralPath $path -PathType Leaf){Remove-Item -LiteralPath $path -Force}
 }
}
