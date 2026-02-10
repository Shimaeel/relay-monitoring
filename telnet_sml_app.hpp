/**
 * @file telnet_sml_app.hpp
 * @brief Telnet-SML DLL application facade
 *
 * @details Provides a single entry point for starting and stopping the
 * Telnet-SML runtime hosted in the DLL.
 */

#pragma once

#include <memory>

#include "dll_export.hpp"

class TELNET_SML_API TelnetSmlApp
{
public:
    TelnetSmlApp();
    ~TelnetSmlApp();

    TelnetSmlApp(const TelnetSmlApp&) = delete;
    TelnetSmlApp& operator=(const TelnetSmlApp&) = delete;

    TelnetSmlApp(TelnetSmlApp&&) = delete;
    TelnetSmlApp& operator=(TelnetSmlApp&&) = delete;

    bool start();
    void waitForExit();
    void stop();
    bool isRunning() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
