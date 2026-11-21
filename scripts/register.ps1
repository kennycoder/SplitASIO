$ErrorActionPreference = "Stop"

$dllPath = Join-Path $PSScriptRoot "..\build\Release\split_asio.dll" | Resolve-Path -ErrorAction SilentlyContinue
if (-not $dllPath) {
    Write-Error "split_asio.dll not found. Build the project first with: cmake --build build --config Release"
    exit 1
}

$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Requesting administrator privileges to register SplitASIO..."
    Start-Process -FilePath "regsvr32.exe" -ArgumentList "`"$($dllPath.Path)`"" -Verb RunAs -Wait
} else {
    & regsvr32.exe "$($dllPath.Path)"
}
