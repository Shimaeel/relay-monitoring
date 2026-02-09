# Build script for telnet_fsm_test
$ErrorActionPreference = "Continue"

Write-Host "Building telnet_fsm_test.exe..." -ForegroundColor Cyan

# Kill any running instances
taskkill /F /IM telnet_fsm_test.exe 2>$null | Out-Null
Start-Sleep -Milliseconds 500

# Compile SQLite as C (if not already done)
if (-not (Test-Path "sqlite3.o")) {
    Write-Host "Compiling sqlite3.c..." -ForegroundColor Yellow
    gcc -c -I C:\sqlite C:\sqlite\sqlite3.c -o sqlite3.o
}

# Compile C++ and link (with big-obj for Boost.Beast headers)
g++ -std=c++17 `
    "-Wa,-mbig-obj" `
    -I third_party/sml/include `
    -I C:\Libraries\boost_1_90_0 `
    -I C:\sqlite `
    main.cpp client.cpp ser_database.cpp sqlite3.o `
    -o telnet_fsm_test.exe `
    -lws2_32 -lmswsock

if ($LASTEXITCODE -eq 0) {
    Write-Host "Build successful!" -ForegroundColor Green
} else {
    Write-Host "Build failed!" -ForegroundColor Red
    exit 1
}
