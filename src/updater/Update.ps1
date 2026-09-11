param([Parameter(Mandatory=$true)][string]$Request)
$ErrorActionPreference = 'Stop'
# A launcher started from PowerShell 7 can inherit its incompatible module paths.
# This worker always uses Windows PowerShell and its own inbox modules.
$env:PSModulePath = "$PSHOME\Modules"
Import-Module (Join-Path $PSScriptRoot 'UpdateEngine.psm1') -Force
Invoke-DxlUpdate -RequestFile $Request
