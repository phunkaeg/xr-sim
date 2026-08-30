[CmdletBinding()]
param(
    [ValidateSet("all", "x86", "x64")][string]$Architecture = "all",
    [ValidateSet("Debug", "RelWithDebInfo", "Release")][string]$Configuration = "RelWithDebInfo",
    [switch]$NoTests
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$architectures = if ($Architecture -eq "all") { @("x86", "x64") } else { @($Architecture) }

# Some process launchers can inject both PATH and Path into the Windows
# environment block. Native tools tolerate that, but the .NET Framework used by
# MSBuild rejects the duplicate case-insensitive key before it can start CL.exe.
# Detect the raw environment (PowerShell's Env: provider hides the duplicate)
# and, only in that case, run CMake through cmd with the uppercase entry removed.
$pathNames = @(
    & cmd.exe /d /c set |
        Where-Object { $_ -match '^(?<Name>[^=]+)=' -and $Matches.Name.ToUpperInvariant() -eq 'PATH' } |
        ForEach-Object { ($_ -split '=', 2)[0] } |
        Sort-Object -CaseSensitive -Unique
)
$sanitizePath = $pathNames.Count -gt 1

function Invoke-BuildTool {
    param([Parameter(Mandatory)][string]$Name, [Parameter(ValueFromRemainingArguments)][string[]]$Arguments)

    if (-not $sanitizePath) {
        & $Name @Arguments | Out-Host
        $exitCode = $LASTEXITCODE
        return $exitCode
    }

    $toolPath = (Get-Command $Name -ErrorAction Stop).Source
    $quotedArguments = @($Arguments | ForEach-Object { '"' + $_.Replace('"', '""') + '"' })
    $command = 'set PATH=& "' + $toolPath + '" ' + ($quotedArguments -join ' ')
    & cmd.exe /d /c $command | Out-Host
    $exitCode = $LASTEXITCODE
    return $exitCode
}

Push-Location $repo
try {
    foreach ($arch in $architectures) {
        $result = Invoke-BuildTool cmake --preset $arch
        if ($result -ne 0) { throw "CMake configure failed for $arch" }
        $result = Invoke-BuildTool cmake --build --preset "$arch-release" --config $Configuration
        if ($result -ne 0) { throw "CMake build failed for $arch" }
        if (-not $NoTests) {
            $result = Invoke-BuildTool ctest --test-dir "build\$arch-vs" -C $Configuration --output-on-failure
            if ($result -ne 0) { throw "CTest failed for $arch" }
        }
    }
} finally {
    Pop-Location
}
