# Publishes the WPF host as a single self-contained win-x64 exe: the recipient needs no
# .NET install, just a double-click.
#
#   powershell -ExecutionPolicy Bypass -File scripts\publish-portable.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\publish-portable.ps1 -Clean
#
# The exe is unsigned, so SmartScreen will intercept the first launch ("More info" ->
# "Run anyway"). Code signing is outside the scope of this repository.
#
# Keep this file ASCII-only. Windows PowerShell 5.1 decodes a BOM-less UTF-8 .ps1 as ANSI,
# and the resulting mojibake can swallow a newline and eat the following line.
param(
    [string]$Configuration = 'Release',
    [string]$OutDir = '',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

# NuGet restore fails under a stripped environment where these are missing.
if (-not [Environment]::GetEnvironmentVariable('PROGRAMFILES')) {
    [Environment]::SetEnvironmentVariable('PROGRAMFILES', 'C:\Program Files')
}
if (-not [Environment]::GetEnvironmentVariable('PROGRAMFILES(X86)')) {
    [Environment]::SetEnvironmentVariable('PROGRAMFILES(X86)', 'C:\Program Files (x86)')
}

$proj = Join-Path $PSScriptRoot '..\src\NimBleImuHost\NimBleImuHost.csproj'
$out  = if ($OutDir) { $OutDir } else { Join-Path $PSScriptRoot '..\publish' }

if ($Clean -and (Test-Path $out)) {
    Remove-Item -Recurse -Force $out
}

# Self-contained: ships the runtime. Single file: one exe instead of a folder of dlls.
# Native libraries must be extracted at run time for WPF to find them, hence the flag.
& dotnet publish $proj -c $Configuration -r win-x64 --self-contained true `
    /p:PublishSingleFile=true `
    /p:IncludeNativeLibrariesForSelfExtract=true `
    /p:EnableCompressionInSingleFile=true `
    -o $out
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed with exit code $LASTEXITCODE" }

$exe = Join-Path $out 'NimBleImuHost.exe'
if (-not (Test-Path $exe)) { throw "Expected artifact not found: $exe" }

$mb = [math]::Round((Get-Item $exe).Length / 1MB, 1)
Get-Item $exe | Select-Object FullName, Length
# Under ~25 MB usually means the runtime was not bundled and the target machine
# will need .NET 8 installed; over ~120 MB usually means compression was skipped.
if ($mb -lt 25 -or $mb -gt 120) {
    Write-Warning "Artifact is ${mb} MB, outside the expected 25-120 MB range for a compressed self-contained single file."
}
Write-Output "Published ${mb} MB -> $exe"
