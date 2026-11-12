# build.ps1 — one-shot MSVC build.
#
#   powershell -ep bypass -f scripts/build.ps1
#
# Attribution: Built by Olivia — https://discord.gg/ojailbreaks

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

Push-Location $root
try {
    $cm = "C:\Program Files\CMake\bin\cmake.exe"
    if (!(Test-Path $cm)) { $cm = "cmake" }

    if (!(Test-Path "build")) { & $cm -S . -B build -G "Visual Studio 17 2022" -A x64 }
    & $cm --build build --config Release

    Write-Host ""
    Write-Host "built: $root\build\bin\Release\MicrosoftEdgeUpdateBroker.exe" -ForegroundColor Green
    Write-Host "run as admin against an offline/sandbox target." -ForegroundColor Yellow
} finally {
    Pop-Location
}
