param([string]$OutDir = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$OutDir = Resolve-DxlOutput $OutDir
Set-Location (Split-Path $PSScriptRoot -Parent)
& ./scripts/build_core.ps1 -OutDir $OutDir -LibraryOnly
& cl.exe /nologo /O2 /MT /EHsc /std:c++20 /utf-8 /Isrc/core tests/final_present_scope.cpp "/Fo$OutDir/obj/final_present_scope.obj" "/Fe$OutDir/final_present_scope.exe"
if ($LASTEXITCODE) { throw 'Final presentation scope compilation failed' }
& "$OutDir/final_present_scope.exe"
if ($LASTEXITCODE) { throw 'Final presentation scope regression failed' }
New-Item -ItemType Directory -Force -Path "$OutDir/obj/grading-pipeline" | Out-Null
$flags = @('/nologo','/O2','/MT','/EHsc','/std:c++20','/utf-8','/W3','/D_CRT_SECURE_NO_WARNINGS',
    '/DUNICODE','/D_UNICODE','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/Isrc/core','/Isrc/common',
    ('/I'+(Join-Path $DependencyRoot 'dlss/include')),'/Ithird_party/fidelityfx/sdk/include',
    '/Ithird_party/fidelityfx/sdk/src/backends/shared',"/Fo$OutDir/obj/grading-pipeline/")
$sources = @('tests/grading_pipeline.cpp','src/core/DlssNrFilter.cpp','src/core/DlssNrFilter11.cpp',
    'src/core/ColorGrading.cpp','src/core/ComputePasses.cpp','src/core/OpticalFlow.cpp',
    'src/core/OpticalFlowShaders.cpp','src/core/CommandListTracker.cpp','src/core/FreezeWatchdog.cpp')
& cl.exe @flags @sources /link "/OUT:$OutDir/grading_pipeline.exe" d3d11.lib d3d12.lib dxgi.lib dxguid.lib `
    d3dcompiler.lib psapi.lib user32.lib shell32.lib ole32.lib windowscodecs.lib advapi32.lib `
    (Join-Path $DependencyRoot 'dlss/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib') "$OutDir/ffx_optical.lib"
if ($LASTEXITCODE) { throw 'Color grading pipeline compilation failed' }
& "$OutDir/grading_pipeline.exe"
if ($LASTEXITCODE) { throw 'Color grading pipeline regression failed' }
