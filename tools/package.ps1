[CmdletBinding()]
param(
    [ValidateSet("RelWithDebInfo", "Release")][string]$Configuration = "RelWithDebInfo",
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $NoBuild) { & (Join-Path $PSScriptRoot "build.ps1") -Architecture all -Configuration $Configuration }
$version = (Select-String -LiteralPath (Join-Path $repo "CMakeLists.txt") -Pattern 'project\(xr-sim VERSION ([\d.]+)').Matches[0].Groups[1].Value
$out = Join-Path $repo "dist\xr-sim-$version"
if (Test-Path $out) { Remove-Item -LiteralPath $out -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($arch in @("x86", "x64")) {
    $dst = Join-Path $out "bin\$arch"
    New-Item -ItemType Directory -Force $dst | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo "build\$arch-vs\bin\$Configuration\xrsim.dll") -Destination $dst -Force
    Copy-Item -LiteralPath (Join-Path $repo "build\$arch-vs\bin\$Configuration\xrsim.pdb") -Destination $dst -Force -ErrorAction SilentlyContinue
}
Copy-Item -LiteralPath (Join-Path $repo "catalog"), (Join-Path $repo "include"), (Join-Path $repo "docs"), (Join-Path $repo "scenarios"), (Join-Path $repo "tools"), (Join-Path $repo "src"), (Join-Path $repo "tests"), (Join-Path $repo "runtime"), (Join-Path $repo "third_party") -Destination $out -Recurse -Force
Copy-Item -LiteralPath (Join-Path $repo "README.md"), (Join-Path $repo "LICENSE"), (Join-Path $repo "NOTICE.md"), (Join-Path $repo "CHANGELOG.md"), (Join-Path $repo "CMakeLists.txt"), (Join-Path $repo "CMakePresets.json") -Destination $out -Force
Compress-Archive -Path "$out\*" -DestinationPath "$out.zip" -Force
Get-Item $out, "$out.zip"
