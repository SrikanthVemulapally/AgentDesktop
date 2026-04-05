# build_and_test.ps1 — run from repo root in a standalone PowerShell window
Set-Location $PSScriptRoot

Write-Host "=== Building ===" -ForegroundColor Cyan
cmake --build build\Release --config Release --parallel
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED" -ForegroundColor Red; exit 1 }

Write-Host "`n=== Unit tests (no virtual desktop) ===" -ForegroundColor Cyan
& bin\AgentDesktopTests.exe --gtest_filter=-Integration.*
$unit = $LASTEXITCODE

Write-Host "`n=== Integration tests (virtual desktop) ===" -ForegroundColor Cyan
& bin\AgentDesktopTests.exe --gtest_filter=Integration.*
$integ = $LASTEXITCODE

if ($unit -eq 0 -and $integ -eq 0) {
    Write-Host "`nALL TESTS PASSED" -ForegroundColor Green
} else {
    Write-Host "`nSOME TESTS FAILED (unit=$unit integ=$integ)" -ForegroundColor Red
}
