param(
    [string]$Configuration = "Release",
    [switch]$Installer,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root "build"

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}

Write-Host "== Configure =="
cmake -S $Root -B $BuildDir -G "Visual Studio 17 2022" -A x64

Write-Host "== Build ($Configuration) =="
cmake --build $BuildDir --config $Configuration --parallel

Write-Host "== Test =="
ctest --test-dir $BuildDir -C $Configuration --output-on-failure

if ($Installer) {
    $isccCandidates = @(
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
        "C:\Program Files\Inno Setup 6\ISCC.exe"
    )
    $iscc = $isccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) {
        throw "ISCC.exe (Inno Setup 6) was not found."
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $Root "dist") | Out-Null
    Write-Host "== Installer =="
    & $iscc (Join-Path $Root "installer\WinConsoleServer.iss")
}

Write-Host "Done."
