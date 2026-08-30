[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [string[]]$ArgumentList = @(),
    [string]$WorkingDirectory = "",
    [ValidateSet("Debug", "RelWithDebInfo", "Release")][string]$Configuration = "RelWithDebInfo",
    [string]$StateDir = "$env:LOCALAPPDATA\xr-sim"
)

$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path $Executable).Path
if (-not $WorkingDirectory) { $WorkingDirectory = Split-Path -Parent $exe }

$stream = [IO.File]::OpenRead($exe)
try {
    $reader = [IO.BinaryReader]::new($stream)
    $stream.Position = 0x3c
    $pe = $reader.ReadInt32()
    $stream.Position = $pe + 4
    $machine = $reader.ReadUInt16()
} finally { $stream.Dispose() }
$arch = switch ($machine) { 0x014c { "x86" } 0x8664 { "x64" } default { throw "unsupported PE machine 0x$($machine.ToString('X4'))" } }

$install = & (Join-Path $PSScriptRoot "install-runtime.ps1") -Architecture $arch -Configuration $Configuration
$savedRuntime = $env:XR_RUNTIME_JSON
$savedDir = $env:XRSIM_DIR
try {
    $env:XR_RUNTIME_JSON = $install.Manifest
    $env:XRSIM_DIR = $StateDir
    if ($ArgumentList.Count) {
        Start-Process -FilePath $exe -ArgumentList $ArgumentList -WorkingDirectory $WorkingDirectory -PassThru
    } else {
        Start-Process -FilePath $exe -WorkingDirectory $WorkingDirectory -PassThru
    }
} finally {
    $env:XR_RUNTIME_JSON = $savedRuntime
    $env:XRSIM_DIR = $savedDir
}
