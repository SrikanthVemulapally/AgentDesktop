# build.ps1 — Build AgentDesktop on Windows (single binary).
# Usage:
#   .\scripts\build.ps1              # Release
#   .\scripts\build.ps1 -Config Debug

param([string]$Config = "Release")
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

Write-Host ""
Write-Host "  ╔══════════════════════════════╗" -ForegroundColor Cyan
Write-Host "  ║   AgentDesktop  Build        ║" -ForegroundColor Cyan
Write-Host "  ╚══════════════════════════════╝" -ForegroundColor Cyan
Write-Host "  Config : $Config"
Write-Host "  Root   : $Root"
Write-Host ""

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Error "cmake not found. Install: winget install Kitware.CMake"
    exit 1
}

$BuildDir = Join-Path $Root "build\$Config"
Write-Host "[1/3] Configuring..." -ForegroundColor Yellow
cmake -S $Root -B $BuildDir -DCMAKE_BUILD_TYPE=$Config
if ($LASTEXITCODE -ne 0) { Write-Error "Configure failed"; exit 1 }

Write-Host ""
Write-Host "[2/3] Building ($Config)..." -ForegroundColor Yellow
cmake --build $BuildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { Write-Error "Build failed"; exit 1 }

Write-Host ""
Write-Host "[3/3] Output:" -ForegroundColor Yellow
$BinDir = Join-Path $Root "bin"
if (Test-Path $BinDir) {
    Get-ChildItem $BinDir -Filter "*.exe" | ForEach-Object {
        Write-Host "  $($_.FullName)  ($([math]::Round($_.Length/1KB,0)) KB)" -ForegroundColor Green
    }
}
Write-Host ""
Write-Host "  Done! Run:  .\bin\AgentDesktop.exe" -ForegroundColor Green
Write-Host ""
