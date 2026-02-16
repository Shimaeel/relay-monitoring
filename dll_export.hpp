// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file dll_export.hpp
 * @brief DLL Export/Import Macros for Windows
 * 
 * @details This header provides cross-platform macros for building and using
 * the Telnet-SML library as a Windows DLL (Dynamic Link Library) or static library.
 * 
 * ## Usage
 * 
 * ### Static Library (default)
 * When building or using as a **static library**, define `TELNET_SML_STATIC`:
 * @code
 * g++ -DTELNET_SML_STATIC main.cpp client.cpp -o app.exe
 * @endcode
 * 
 * ### Building the DLL
 * When **building** the DLL, define `TELNET_SML_EXPORTS`:
 * @code
 * g++ -DTELNET_SML_EXPORTS -shared -o telnet_sml.dll ...
 * @endcode
 * 
 * ### Using the DLL
 * When **using** the DLL in your application, don't define either macro:
 * @code
 * g++ -o myapp.exe myapp.cpp -L. -ltelnet_sml
 * @endcode
 * 
 * ## Macro Reference
 * 
 * | Define | Purpose |
 * |--------|---------|
 * | TELNET_SML_STATIC | Static library (no dllimport/dllexport) |
 * | TELNET_SML_EXPORTS | Building the DLL (dllexport) |
 * | (neither) | Using the DLL (dllimport) |
 * 
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

// Windows DLL export/import macros
#ifdef _WIN32
    #ifdef TELNET_SML_STATIC
        // Static library - no decoration needed
        #define TELNET_SML_API
    #elif defined(TELNET_SML_EXPORTS)
        // Building the DLL - export symbols
        #define TELNET_SML_API __declspec(dllexport)
    #else
        // Using the DLL - import symbols
        #define TELNET_SML_API __declspec(dllimport)
    #endif
#else
    // Non-Windows platforms - no special decoration needed
    #define TELNET_SML_API
#endif

// For C-style exports (if needed)
#ifdef __cplusplus
    #define TELNET_SML_C_API extern "C" TELNET_SML_API
#else
    #define TELNET_SML_C_API TELNET_SML_API
#endif
