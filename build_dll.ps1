# Build script for Telnet-SML DLL
# Creates telnet_sml.dll and telnet_sml.lib (import library)
$ErrorActionPreference = "Continue"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Building Telnet-SML DLL" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Configuration
$DLL_NAME = "telnet_sml"
$OUTPUT_DIR = "lib"

# Create output directory
if (-not (Test-Path $OUTPUT_DIR)) {
    New-Item -ItemType Directory -Path $OUTPUT_DIR | Out-Null
}

# Kill any running processes using the DLL
taskkill /F /IM telnet_fsm_test.exe 2>$null | Out-Null
Start-Sleep -Milliseconds 300

# Step 1: Compile SQLite as C (if not already done)
Write-Host "[1/4] Compiling SQLite..." -ForegroundColor Yellow
if (-not (Test-Path "sqlite3.o")) {
    gcc -c -I C:\sqlite C:\sqlite\sqlite3.c -o sqlite3.o
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Failed to compile SQLite!" -ForegroundColor Red
        exit 1
    }
}
Write-Host "      SQLite ready" -ForegroundColor Green

# Step 2: Compile C++ sources to object files
Write-Host "[2/4] Compiling C++ sources..." -ForegroundColor Yellow

$CXX_FLAGS = @(
    "-std=c++17",
    "-Wa,-mbig-obj",
    "-DTELNET_SML_EXPORTS",          # Enable DLL export
    "-I third_party/sml/include",
    "-I C:\Libraries\boost_1_90_0",
    "-I C:\sqlite"
)

$SOURCES = @(
    "client.cpp",
    "ser_database.cpp",
    "telnet_sml_app.cpp"
)

$OBJECTS = @()

foreach ($src in $SOURCES) {
    $obj = $src -replace "\.cpp$", ".o"
    Write-Host "      Compiling $src..." -ForegroundColor Gray
    
    & g++ @CXX_FLAGS -c $src -o $obj
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Failed to compile $src!" -ForegroundColor Red
        exit 1
    }
    $OBJECTS += $obj
}
Write-Host "      C++ sources compiled" -ForegroundColor Green

# Step 3: Link DLL
Write-Host "[3/4] Linking DLL..." -ForegroundColor Yellow

$LINK_FLAGS = @(
    "-shared",
    "-o", "$OUTPUT_DIR\$DLL_NAME.dll",
    "-Wl,--out-implib,$OUTPUT_DIR\lib$DLL_NAME.a",   # Import library for MinGW
    "-Wl,--export-all-symbols"
)

$ALL_OBJECTS = $OBJECTS + @("sqlite3.o")
$LIBS = @("-lws2_32", "-lmswsock")

& g++ @CXX_FLAGS @LINK_FLAGS @ALL_OBJECTS @LIBS

if ($LASTEXITCODE -ne 0) {
    Write-Host "Failed to link DLL!" -ForegroundColor Red
    exit 1
}
Write-Host "      DLL created: $OUTPUT_DIR\$DLL_NAME.dll" -ForegroundColor Green

# Step 4: Create def file for MSVC compatibility (optional)
Write-Host "[4/4] Generating exports..." -ForegroundColor Yellow

# Use gendef if available (from mingw-w64-tools)
if (Get-Command gendef -ErrorAction SilentlyContinue) {
    gendef "$OUTPUT_DIR\$DLL_NAME.dll"
    Move-Item -Force "$DLL_NAME.def" "$OUTPUT_DIR\" -ErrorAction SilentlyContinue
    Write-Host "      DEF file created for MSVC" -ForegroundColor Green
} else {
    Write-Host "      gendef not found - skipping DEF file" -ForegroundColor Gray
}

# Clean up object files
Remove-Item *.o -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "  Build Successful!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "Output files:" -ForegroundColor Cyan
Write-Host "  DLL:    $OUTPUT_DIR\$DLL_NAME.dll" -ForegroundColor White
Write-Host "  Import: $OUTPUT_DIR\lib$DLL_NAME.a" -ForegroundColor White
Write-Host ""
Write-Host "Usage in your application:" -ForegroundColor Yellow
Write-Host '  g++ -std=c++17 your_app.cpp -Llib -ltelnet_sml -o your_app.exe' -ForegroundColor Gray
Write-Host ""
