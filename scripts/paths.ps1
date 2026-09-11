# Development output and proprietary dependencies always live outside source.
$script:SourceRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$script:WorkspaceRoot = if ($env:DXL_WORKSPACE) { [IO.Path]::GetFullPath($env:DXL_WORKSPACE) } else { [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $script:SourceRoot) 'DXL-Workspace')) }
if ($script:WorkspaceRoot.Equals($script:SourceRoot,[StringComparison]::OrdinalIgnoreCase) -or $script:WorkspaceRoot.StartsWith($script:SourceRoot+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'DXL_WORKSPACE must be outside the source directory.' }
$script:DependencyRoot = Join-Path $script:WorkspaceRoot 'dependencies'
$script:BuildRoot = Join-Path $script:WorkspaceRoot 'build'
$env:DXL_WORKSPACE = $script:WorkspaceRoot
function Resolve-DxlOutput([string]$Path, [string]$Default = 'dxl-0.1') {
    if (!$Path) { $Path = Join-Path $script:BuildRoot $Default }
    elseif (![IO.Path]::IsPathRooted($Path)) { $Path = Join-Path $script:BuildRoot ($Path -replace '^build[\\/]','') }
    $resolved = [IO.Path]::GetFullPath($Path)
    if ($resolved.Equals($script:SourceRoot,[StringComparison]::OrdinalIgnoreCase) -or $resolved.StartsWith($script:SourceRoot+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Build/test output must be outside source.' }
    return $resolved
}
