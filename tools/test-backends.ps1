[CmdletBinding()]
param(
    [ValidateSet("all", "x86", "x64")][string]$Architecture = "all",
    [ValidateSet("Debug", "RelWithDebInfo", "Release")][string]$Configuration = "RelWithDebInfo",
    [string[]]$Backends = @("headless", "d3d9", "d3d10", "d3d11", "d3d12", "opengl", "vulkan")
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$architectures = if ($Architecture -eq "all") { @("x86", "x64") } else { @($Architecture) }
$failures = @()
$skips = @()

foreach ($arch in $architectures) {
    $bin = Join-Path $repo "build\$arch-vs\bin\$Configuration"
    $probe = Join-Path $bin "xrsim_probe.exe"
    $runtime = Join-Path $bin "xrsim.dll"
    if (-not (Test-Path $probe) -or -not (Test-Path $runtime)) {
        throw "missing $arch build; run .\tools\build.ps1 first"
    }
    foreach ($backend in $Backends) {
        $state = Join-Path $repo "build\test-state\$arch-$backend"
        New-Item -ItemType Directory -Force $state | Out-Null
        $saved = $env:XRSIM_DIR
        try {
            $env:XRSIM_DIR = $state
            & $probe $runtime $backend
            $code = $LASTEXITCODE
        } finally {
            $env:XRSIM_DIR = $saved
        }
        if ($code -eq 77) { $skips += "$arch/$backend" }
        elseif ($code -ne 0) { $failures += "$arch/$backend (exit $code)" }
    }
}

if ($skips.Count) { Write-Host "Skipped (device/driver unavailable): $($skips -join ', ')" -ForegroundColor Yellow }
if ($failures.Count) { throw "backend failures: $($failures -join ', ')" }
Write-Host "Renderer matrix passed." -ForegroundColor Green
