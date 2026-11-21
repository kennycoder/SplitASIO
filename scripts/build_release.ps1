$ErrorActionPreference = "Stop"

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
Push-Location $root
try {
    Write-Host "=== 1. Configuring & building Release x64 ===" -ForegroundColor Cyan
    cmake -B build -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

    cmake --build build --config Release --target split_asio split_asio_config
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }

    Write-Host "`n=== 2. Locating Inno Setup 6 (ISCC.exe) ===" -ForegroundColor Cyan
    $isccCandidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
        "${env:LOCALAPPDATA}\Programs\Inno Setup 6\ISCC.exe"
    )
    $iscc = $isccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) {
        $cmd = Get-Command iscc -ErrorAction SilentlyContinue
        if ($cmd) { $iscc = $cmd.Source }
    }

    if (-not $iscc) {
        Write-Warning "Inno Setup 6 (ISCC.exe) not found."
        Write-Host "Install it once with:  winget install JRSoftware.InnoSetup"
        Write-Host "Then re-run:           .\scripts\build_release.ps1"
        exit 1
    }

    Write-Host "Found ISCC: $iscc"
    Write-Host "`n=== 3. Building Installer ===" -ForegroundColor Cyan
    & $iscc "installer\split_asio.iss"
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed." }

    $setup = Get-Item "dist\SplitASIO-Setup-*.exe" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($setup) {
        $hash = (Get-FileHash $setup.FullName -Algorithm SHA256).Hash
        $sizeKB = [math]::Round($setup.Length / 1KB, 1)
        Write-Host "`n=== Release Ready ===" -ForegroundColor Green
        Write-Host "File:   $($setup.FullName)"
        Write-Host "Size:   $sizeKB KB"
        Write-Host "SHA256: $hash"
    }
} finally {
    Pop-Location
}
