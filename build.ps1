# Build script for Telnet-SML
# Supports both static executable and DLL builds

param(
    [switch]$DLL,           # Build as DLL instead of static executable
    [switch]$Example,       # Build DLL example application
    [switch]$Clean          # Clean build artifacts
)

$ErrorActionPreference = "Continue"

if ($Clean) {
    Remove-Item *.o, *.exe -ErrorAction SilentlyContinue
    Remove-Item lib\* -ErrorAction SilentlyContinue
    Write-Host "Clean complete." -ForegroundColor Green
    exit 0
}

$BOOST_INC = "C:\Libraries\boost_1_90_0"
$SQLITE_INC = "C:\sqlite"
$SML_INC = "third_party/sml/include"

taskkill /F /IM telnet_fsm_test.exe 2>$null | Out-Null
taskkill /F /IM dll_example.exe 2>$null | Out-Null
Start-Sleep -Milliseconds 300

if (-not (Test-Path "sqlite3.o")) {
    & gcc -c "-I$SQLITE_INC" "$SQLITE_INC\sqlite3.c" -o sqlite3.o
    if ($LASTEXITCODE -ne 0) { Write-Host "SQLite compile failed." -ForegroundColor Red; exit 1 }
}

if ($DLL) {
    $OUTPUT_DIR = "lib"
    if (-not (Test-Path $OUTPUT_DIR)) { New-Item -ItemType Directory -Path $OUTPUT_DIR | Out-Null }

    & g++ -std=c++17 "-Wa,-mbig-obj" `
        "-I$SML_INC" "-I$BOOST_INC" "-I$SQLITE_INC" `
        "-Iwebsocket API" "-Ishared_memory" "-I." `
        -DTELNET_SML_EXPORTS `
        -shared -o "$OUTPUT_DIR/telnet_sml.dll" `
        "-Wl,--out-implib,$OUTPUT_DIR/libtelnet_sml.a" `
        "-Wl,--export-all-symbols" `
        client.cpp ser_database.cpp telnet_sml_app.cpp sqlite3.o `
        -lws2_32 -lmswsock

    if ($LASTEXITCODE -eq 0) { Write-Host "DLL build OK -> $OUTPUT_DIR\telnet_sml.dll" -ForegroundColor Green }
    else { Write-Host "DLL build failed." -ForegroundColor Red; exit 1 }
}
elseif ($Example) {
    if (-not (Test-Path "lib\telnet_sml.dll")) {
        Write-Host "DLL not found. Run './build.ps1 -DLL' first." -ForegroundColor Red; exit 1
    }

    & g++ -std=c++17 "-Wa,-mbig-obj" `
        "-I$SML_INC" "-I$BOOST_INC" "-I$SQLITE_INC" `
        "-Iwebsocket API" "-Ishared_memory" "-I." `
        dll_example.cpp `
        -Llib -ltelnet_sml `
        -o dll_example.exe `
        -lws2_32 -lmswsock

    if ($LASTEXITCODE -eq 0) {
        Copy-Item "lib\telnet_sml.dll" -Destination "." -Force
        Write-Host "Example build OK -> dll_example.exe" -ForegroundColor Green
    } else { Write-Host "Example build failed." -ForegroundColor Red; exit 1 }
}
else {
    & g++ -std=c++17 "-Wa,-mbig-obj" `
        -DTELNET_SML_STATIC `
        "-I$SML_INC" "-I$BOOST_INC" "-I$SQLITE_INC" `
        "-Iwebsocket API" "-Ishared_memory" "-I." `
        main.cpp client.cpp ser_database.cpp telnet_sml_app.cpp sqlite3.o `
        -o telnet_fsm_test.exe `
        -lws2_32 -lmswsock

    if ($LASTEXITCODE -eq 0) { Write-Host "Build OK -> telnet_fsm_test.exe" -ForegroundColor Green }
    else { Write-Host "Build failed." -ForegroundColor Red; exit 1 }
}
