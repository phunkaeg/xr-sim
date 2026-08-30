[CmdletBinding()]
param(
    [ValidateSet("x86", "x64")][string]$Architecture = $(if ([Environment]::Is64BitProcess) { "x64" } else { "x86" }),
    [ValidateSet("Debug", "RelWithDebInfo", "Release")][string]$Configuration = "RelWithDebInfo",
    [string]$Dir = "$env:LOCALAPPDATA\xr-sim\runtime"
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$sourceDll = Join-Path $repo "build\$Architecture-vs\bin\$Configuration\xrsim.dll"
if (-not (Test-Path $sourceDll)) {
    $sourceDll = Join-Path $repo "bin\$Architecture\xrsim.dll"
}
if (-not (Test-Path $sourceDll)) { throw "runtime not built: $sourceDll" }

$archDir = Join-Path $Dir $Architecture
New-Item -ItemType Directory -Force $archDir | Out-Null
$dll = Join-Path $archDir "xrsim.dll"
Copy-Item -LiteralPath $sourceDll -Destination $dll -Force

$manifest = Join-Path $Dir "xrsim-$Architecture.json"
$escaped = $dll.Replace('\', '\\')
$json = '{"file_format_version":"1.0.0","runtime":{"name":"xr-sim","library_path":"' + $escaped + '"}}'
[IO.File]::WriteAllText($manifest, $json, [Text.UTF8Encoding]::new($false))

[pscustomobject]@{
    Architecture = $Architecture
    Dll = $dll
    Manifest = $manifest
    Command = "`$env:XR_RUNTIME_JSON = '$manifest'"
}
