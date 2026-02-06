# Build script for telnet_fsm_test
$ErrorActionPreference = "Stop"

Write-Host "Building telnet_fsm_test.exe..." -ForegroundColor Cyan

# Kill any running instances
taskkill /F /IM telnet_fsm_test.exe 2>$null | Out-Null
Start-Sleep -Milliseconds 500

# Compile
g++ -std=c++17 `
    -I third_party/sml/include `
    -I C:\Development\Libraries\boost_1_90_0 `
    main.cpp client.cpp `
    -o telnet_fsm_test.exe `
    -lws2_32

if ($LASTEXITCODE -eq 0) {
    Write-Host "Build successful!" -ForegroundColor Green
} else {
    Write-Host "Build failed!" -ForegroundColor Red
    exit 1
}
