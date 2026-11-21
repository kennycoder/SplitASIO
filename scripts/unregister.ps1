$ErrorActionPreference = "Stop"

$dllPath = Join-Path $PSScriptRoot "..\build\Release\split_asio.dll" | Resolve-Path -ErrorAction SilentlyContinue
if (-not $dllPath) {
    Write-Error "split_asio.dll not found."
    exit 1
}

$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Requesting administrator privileges to unregister SplitASIO..."
    Start-Process -FilePath "regsvr32.exe" -ArgumentList "/u `"$($dllPath.Path)`"" -Verb RunAs -Wait
} else {
    & regsvr32.exe /u "$($dllPath.Path)"
}
