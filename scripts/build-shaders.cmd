@echo off
rem Compile the HLSL into precompiled bytecode headers, checked into the repo.
rem ASCII-only content on purpose -- cmd mis-parses non-ASCII batch files.
rem Generated source assets are intentionally written under src/.
rem
rem Why not compile at runtime: core is injected into someone else's game
rem process, so fewer dependencies is safer. Runtime compilation would drag in
rem d3dcompiler_47.dll and run a compiler inside the game. These shaders change
rem about once a year. OptiScaler does the same (dxc -> .cso -> .h).
rem
rem fxc from the Windows SDK is enough (cs_5_0, plain per-pixel math). We
rem deliberately do not use the dxc.exe bundled in OptiScaler's repo.
setlocal

set "SRC=%~dp0..\src\core\shaders"
set "OUT=%SRC%\precompiled"
if not exist "%OUT%" mkdir "%OUT%"

set "FXC="
for /f "delims=" %%d in ('dir /b /o-n "%ProgramFiles(x86)%\Windows Kits\10\bin" 2^>nul') do (
  if exist "%ProgramFiles(x86)%\Windows Kits\10\bin\%%d\x64\fxc.exe" (
    if not defined FXC set "FXC=%ProgramFiles(x86)%\Windows Kits\10\bin\%%d\x64\fxc.exe"
  )
)
if not defined FXC (
  echo Could not find fxc.exe in the Windows SDK.
  exit /b 1
)
echo Using %FXC%

echo Compiling ComputePasses : EncodeMain
"%FXC%" /nologo /T cs_5_0 /E EncodeMain /O3 /Qstrip_debug /Qstrip_reflect ^
  /Fh "%OUT%\ToneEncode.h" /Vn g_toneEncodeCs "%SRC%\ComputePasses.hlsl"
if errorlevel 1 exit /b 1

echo Compiling ComputePasses : DecodeMain
"%FXC%" /nologo /T cs_5_0 /E DecodeMain /O3 /Qstrip_debug /Qstrip_reflect ^
  /Fh "%OUT%\ToneDecode.h" /Vn g_toneDecodeCs "%SRC%\ComputePasses.hlsl"
if errorlevel 1 exit /b 1

echo Compiling ComputePasses : DecodeRatioMain
"%FXC%" /nologo /T cs_5_0 /E DecodeRatioMain /O3 /Qstrip_debug /Qstrip_reflect ^
  /Fh "%OUT%\ToneDecodeRatio.h" /Vn g_toneDecodeRatioCs "%SRC%\ComputePasses.hlsl"
if errorlevel 1 exit /b 1

echo Compiling ComputePasses : ResampleMain
"%FXC%" /nologo /T cs_5_0 /E ResampleMain /O3 /Qstrip_debug /Qstrip_reflect ^
  /Fh "%OUT%\MotionResample.h" /Vn g_motionResampleCs "%SRC%\ComputePasses.hlsl"
if errorlevel 1 exit /b 1

echo Compiling ComputePasses : VisualizeMain
"%FXC%" /nologo /T cs_5_0 /E VisualizeMain /O3 /Qstrip_debug /Qstrip_reflect ^
  /Fh "%OUT%\Visualize.h" /Vn g_visualizeCs "%SRC%\ComputePasses.hlsl"
if errorlevel 1 exit /b 1

echo Compiling ComputePasses : RatioMakeMain
"%FXC%" /nologo /T cs_5_0 /E RatioMakeMain /O3 /Qstrip_debug /Qstrip_reflect ^
  /Fh "%OUT%\RatioMake.h" /Vn g_ratioMakeCs "%SRC%\ComputePasses.hlsl"
if errorlevel 1 exit /b 1

echo Compiling ComputePasses : RatioApplyMain
"%FXC%" /nologo /T cs_5_0 /E RatioApplyMain /O3 /Qstrip_debug /Qstrip_reflect ^
  /Fh "%OUT%\RatioApply.h" /Vn g_ratioApplyCs "%SRC%\ComputePasses.hlsl"
if errorlevel 1 exit /b 1

echo.
echo Compiling ColorGrading : GradeMain
"%FXC%" /nologo /T cs_5_0 /E GradeMain /O3 /Qstrip_debug /Qstrip_reflect ^
  /Fh "%OUT%\ColorGrading.h" /Vn g_colorGradingCs "%SRC%\ColorGrading.hlsl"
if errorlevel 1 exit /b 1

echo Compiling Bloom : BloomDownsample
"%FXC%" /nologo /T cs_5_0 /E BloomDownsample /O3 /Qstrip_debug /Qstrip_reflect ^
  /Fh "%OUT%\Bloom.h" /Vn g_bloomDownsampleCs "%SRC%\Bloom.hlsl"
if errorlevel 1 exit /b 1

echo OK: %OUT%
exit /b 0
