# Build script for Telnet-SML
# Supports both static executable and DLL builds

param(
    [switch]$DLL,           # Build as DLL instead of static executable
    [switch]$Example,       # Build DLL example application
    [switch]$Clean          # Clean build artifacts
)

$ErrorActionPreference = "Continue"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Telnet-SML Build System" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Clean build
if ($Clean) {
    Write-Host "Cleaning build artifacts..." -ForegroundColor Yellow
    Remove-Item *.o, *.exe -ErrorAction SilentlyContinue
    Remove-Item lib\* -ErrorAction SilentlyContinue
    Write-Host "Clean complete." -ForegroundColor Green
    exit 0
}

# Common configuration  
$BOOST_INC = "C:\Libraries\boost_1_90_0"
$SQLITE_INC = "C:\sqlite"
$SML_INC = "third_party/sml/include"

# Kill any running instances
taskkill /F /IM telnet_fsm_test.exe 2>$null | Out-Null
taskkill /F /IM dll_example.exe 2>$null | Out-Null
Start-Sleep -Milliseconds 300

# Compile SQLite
if (-not (Test-Path "sqlite3.o")) {
    Write-Host "Compiling sqlite3.c..." -ForegroundColor Yellow
    & gcc -c "-I$SQLITE_INC" "$SQLITE_INC\sqlite3.c" -o sqlite3.o
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Failed to compile SQLite!" -ForegroundColor Red
        exit 1
    }
}

if ($DLL) {
    # ========== DLL BUILD ==========
    Write-Host "Building DLL..." -ForegroundColor Cyan
    
    $OUTPUT_DIR = "lib"
    if (-not (Test-Path $OUTPUT_DIR)) {
        New-Item -ItemType Directory -Path $OUTPUT_DIR | Out-Null
    }
    
    & g++ -std=c++17 "-Wa,-mbig-obj" `
        "-I$SML_INC" "-I$BOOST_INC" "-I$SQLITE_INC" `
        "-Iwebsocket API" "-Ishared_memory" "-I." `
        -DTELNET_SML_EXPORTS `
        -shared -o "$OUTPUT_DIR/telnet_sml.dll" `
        "-Wl,--out-implib,$OUTPUT_DIR/libtelnet_sml.a" `
        "-Wl,--export-all-symbols" `
        client.cpp ser_database.cpp telnet_sml_app.cpp sqlite3.o `
        -lws2_32 -lmswsock
    
    if ($LASTEXITCODE -eq 0) {
        Write-Host ""
        Write-Host "DLL build successful!" -ForegroundColor Green
        Write-Host "  Output: $OUTPUT_DIR\telnet_sml.dll" -ForegroundColor White
        Write-Host "  Import: $OUTPUT_DIR\libtelnet_sml.a" -ForegroundColor White
    } else {
        Write-Host "DLL build failed!" -ForegroundColor Red
        exit 1
    }
}
elseif ($Example) {
    # ========== EXAMPLE BUILD (uses DLL) ==========
    Write-Host "Building DLL example application..." -ForegroundColor Cyan
    
    if (-not (Test-Path "lib\telnet_sml.dll")) {
        Write-Host "DLL not found! Run './build.ps1 -DLL' first." -ForegroundColor Red
        exit 1
    }
    
    & g++ -std=c++17 "-Wa,-mbig-obj" `
        "-I$SML_INC" "-I$BOOST_INC" "-I$SQLITE_INC" `
        "-Iwebsocket API" "-Ishared_memory" "-I." `
        dll_example.cpp `
        -Llib -ltelnet_sml `
        -o dll_example.exe `
        -lws2_32 -lmswsock
    
    if ($LASTEXITCODE -eq 0) {
        # Copy DLL to executable directory
        Copy-Item "lib\telnet_sml.dll" -Destination "." -Force
        Write-Host ""
        Write-Host "Example build successful!" -ForegroundColor Green
        Write-Host "  Output: dll_example.exe" -ForegroundColor White
        Write-Host "  Run: ./dll_example.exe" -ForegroundColor Gray
    } else {
        Write-Host "Example build failed!" -ForegroundColor Red
        exit 1
    }
}
else {
    # ========== STATIC BUILD (default) ==========
    Write-Host "Building static executable..." -ForegroundColor Cyan

    & g++ -std=c++17 "-Wa,-mbig-obj" `
        -DTELNET_SML_STATIC `
        "-I$SML_INC" "-I$BOOST_INC" "-I$SQLITE_INC" `
        "-Iwebsocket API" "-Ishared_memory" "-I." `
        main.cpp client.cpp ser_database.cpp telnet_sml_app.cpp sqlite3.o `
        -o telnet_fsm_test.exe `
        -lws2_32 -lmswsock

    if ($LASTEXITCODE -eq 0) {
        Write-Host ""
        Write-Host "Build successful!" -ForegroundColor Green
        Write-Host "  Output: telnet_fsm_test.exe" -ForegroundColor White
    } else {
        Write-Host "Build failed!" -ForegroundColor Red
        exit 1
    }
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Build Options:" -ForegroundColor Cyan
Write-Host "    ./build.ps1          - Static executable" -ForegroundColor Gray
Write-Host "    ./build.ps1 -DLL     - Build DLL library" -ForegroundColor Gray
Write-Host "    ./build.ps1 -Example - Build DLL example" -ForegroundColor Gray
Write-Host "    ./build.ps1 -Clean   - Clean artifacts" -ForegroundColor Gray
Write-Host "========================================" -ForegroundColor Cyan

# Auto-start serve.ps1 for relay-app (COOP/COEP headers for SharedArrayBuffer)
$servePath = Join-Path $PSScriptRoot "relay-app\serve.ps1"
if (Test-Path $servePath) {
    Write-Host ""
    Write-Host "Starting relay-app HTTP server (port 3000)..." -ForegroundColor Yellow
    Start-Process powershell -ArgumentList "-NoExit", "-Command", "cd '$PSScriptRoot\relay-app'; .\serve.ps1"
    Write-Host "  relay-app server started in new window" -ForegroundColor Green
    Write-Host "  Open: http://localhost:3000/index.html" -ForegroundColor Cyan
}
