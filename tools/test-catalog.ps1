[CmdletBinding()]
param(
    [ValidateSet("all", "x86", "x64")][string]$Architecture = "all",
    [ValidateSet("Debug", "RelWithDebInfo", "Release")][string]$Configuration = "RelWithDebInfo",
    [string]$CatalogRoot = "D:\Dev Debug",
    [string[]]$Profile = @(),
    [switch]$External
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$catalogPath = Join-Path $repo "catalog\profiles.json"
$catalog = Get-Content -LiteralPath $catalogPath -Raw | ConvertFrom-Json
$profiles = @($catalog.profiles)
if ($Profile.Count) {
    $profiles = @($profiles | Where-Object { $Profile -contains $_.id })
    $unknown = @($Profile | Where-Object { $_ -notin $profiles.id })
    if ($unknown.Count) { throw "unknown catalog profile(s): $($unknown -join ', ')" }
}
if ($Architecture -ne 'all') {
    $profiles = @($profiles | Where-Object { $_.architecture -eq $Architecture })
}
if (-not $profiles.Count) { throw "no catalog profiles matched the requested filters" }

$failures = @()
$skips = @()

function Get-BuildPaths([string]$Arch) {
    $bin = Join-Path $repo "build\$Arch-vs\bin\$Configuration"
    $probe = Join-Path $bin "xrsim_probe.exe"
    $runtime = Join-Path $bin "xrsim.dll"
    if (-not (Test-Path -LiteralPath $probe) -or -not (Test-Path -LiteralPath $runtime)) {
        throw "missing $Arch build; run .\tools\build.ps1 -Architecture all first"
    }
    return [pscustomobject]@{ Probe = $probe; Runtime = $runtime }
}

function New-RuntimeManifest([string]$Arch, [string]$Runtime) {
    $dir = Join-Path $repo "build\catalog-runtime"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $path = Join-Path $dir "xrsim-$Arch.json"
    $document = [ordered]@{
        file_format_version = '1.0.0'
        runtime = [ordered]@{ name = 'xr-sim'; library_path = $Runtime }
    }
    [IO.File]::WriteAllText($path, ($document | ConvertTo-Json -Compress),
        [Text.UTF8Encoding]::new($false))
    return $path
}

Write-Host "Catalog contract tests" -ForegroundColor Cyan
foreach ($entry in $profiles) {
    $paths = Get-BuildPaths $entry.architecture
    $state = Join-Path $repo "build\catalog-state\contract-$($entry.id)"
    New-Item -ItemType Directory -Force -Path $state | Out-Null
    $savedDir = $env:XRSIM_DIR
    try {
        $env:XRSIM_DIR = $state
        & $paths.Probe $paths.Runtime $entry.openxrBackend
        $code = $LASTEXITCODE
    } finally {
        $env:XRSIM_DIR = $savedDir
    }
    if ($code -eq 77) {
        $skips += "$($entry.id): internal $($entry.architecture)/$($entry.openxrBackend) unavailable"
        Write-Host "SKIP $($entry.id)" -ForegroundColor Yellow
    } elseif ($code -ne 0) {
        $failures += "$($entry.id): internal contract exit $code"
        Write-Host "FAIL $($entry.id)" -ForegroundColor Red
    } else {
        Write-Host "PASS $($entry.id): $($entry.architecture)/$($entry.openxrBackend)" -ForegroundColor Green
    }
}

if ($External) {
    Write-Host "External catalog client probes (games are never launched)" -ForegroundColor Cyan
    foreach ($entry in $profiles) {
        foreach ($externalProbe in @($entry.externalProbes)) {
            $exe = $null
            foreach ($candidate in @($externalProbe.pathCandidates)) {
                $path = Join-Path $CatalogRoot $candidate
                if (Test-Path -LiteralPath $path) { $exe = (Resolve-Path -LiteralPath $path).Path; break }
            }
            if (-not $exe) {
                $skips += "$($entry.id): external probe binary not found"
                Write-Host "SKIP $($entry.id): external probe not built" -ForegroundColor Yellow
                continue
            }

            $paths = Get-BuildPaths $entry.architecture
            $manifest = New-RuntimeManifest $entry.architecture $paths.Runtime
            $state = Join-Path $repo "build\catalog-state\external-$($entry.id)"
            New-Item -ItemType Directory -Force -Path $state | Out-Null
            $savedRuntime = $env:XR_RUNTIME_JSON
            $savedDir = $env:XRSIM_DIR
            Push-Location (Split-Path -Parent $exe)
            try {
                $env:XR_RUNTIME_JSON = $manifest
                $env:XRSIM_DIR = $state
                & $exe @($externalProbe.arguments)
                $code = $LASTEXITCODE
            } finally {
                Pop-Location
                $env:XR_RUNTIME_JSON = $savedRuntime
                $env:XRSIM_DIR = $savedDir
            }
            if ($code -ne [int]$externalProbe.expectedExitCode) {
                $failures += "$($entry.id): external probe exit $code"
                Write-Host "FAIL $($entry.id): external probe exit $code" -ForegroundColor Red
            } else {
                Write-Host "PASS $($entry.id): external client probe" -ForegroundColor Green
            }
        }
    }
}

if ($skips.Count) { Write-Host "Skipped: $($skips -join '; ')" -ForegroundColor Yellow }
if ($failures.Count) { throw "catalog failures: $($failures -join '; ')" }
Write-Host "Catalog validation passed for $($profiles.Count) profile(s)." -ForegroundColor Green
