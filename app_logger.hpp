// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file app_logger.hpp
 * @brief Simple file logger with size-based rotation for 24/7 operation.
 *
 * @details Provides a thread-safe logging utility that writes to a file
 * and automatically rotates when the file exceeds a configurable size.
 * Keeps N rotated files (app.log, app.log.1, app.log.2, ...).
 *
 * Also installs a tee-style streambuf on std::cout / std::cerr so that
 * existing code using std::cout continues to work without modification,
 * while output is also captured to the log file.
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <cstdio>

/**
 * @class AppLogger
 * @brief Size-rotating file logger that tees std::cout and std::cerr.
 *
 * Call AppLogger::init() once at startup. All subsequent std::cout / std::cerr
 * output is duplicated to the log file. When the file exceeds maxBytes,
 * it is rotated (app.log -> app.log.1 -> app.log.2 ... up to maxFiles).
 */
class AppLogger
{
public:
    /**
     * @brief Initialise the logger and install stream tees.
     *
     * @param filePath   Log file path (e.g. "app.log").
     * @param maxBytes   Max file size before rotation (default 5 MB).
     * @param maxFiles   Number of rotated files to keep (default 3).
     */
    static void init(const std::string& filePath = "app.log",
                     std::size_t maxBytes = 5 * 1024 * 1024,
                     int maxFiles = 3)
    {
        auto& inst = instance();
        std::lock_guard<std::mutex> lock(inst.mutex_);

        inst.path_     = filePath;
        inst.maxBytes_ = maxBytes;
        inst.maxFiles_ = maxFiles;

        inst.file_.open(filePath, std::ios::app);
        if (!inst.file_.is_open())
        {
            std::cerr << "[Logger] Failed to open log file: " << filePath << "\n";
            return;
        }

        // Install tee streambufs on cout and cerr
        inst.coutTee_ = std::make_unique<TeeBuf>(std::cout.rdbuf(), inst);
        inst.cerrTee_ = std::make_unique<TeeBuf>(std::cerr.rdbuf(), inst);

        inst.origCout_ = std::cout.rdbuf(inst.coutTee_.get());
        inst.origCerr_ = std::cerr.rdbuf(inst.cerrTee_.get());

        inst.installed_ = true;
    }

    /**
     * @brief Shutdown the logger and restore original streams.
     */
    static void shutdown()
    {
        auto& inst = instance();
        std::lock_guard<std::mutex> lock(inst.mutex_);

        if (inst.installed_)
        {
            std::cout.rdbuf(inst.origCout_);
            std::cerr.rdbuf(inst.origCerr_);
            inst.coutTee_.reset();
            inst.cerrTee_.reset();
            inst.installed_ = false;
        }

        if (inst.file_.is_open())
            inst.file_.close();
    }

private:
    AppLogger() = default;
    ~AppLogger() { shutdown(); }

    static AppLogger& instance()
    {
        static AppLogger inst;
        return inst;
    }

    /// Rotate app.log -> app.log.1 -> app.log.2 ...
    void rotate()
    {
        file_.close();

        for (int i = maxFiles_ - 1; i >= 1; --i)
        {
            std::string src = path_ + (i == 1 ? "" : "." + std::to_string(i - 1));
            if (i == 1) src = path_;
            std::string dst = path_ + "." + std::to_string(i);
            std::remove(dst.c_str());
            std::rename(src.c_str(), dst.c_str());
        }

        file_.open(path_, std::ios::trunc);
        bytesWritten_ = 0;
    }

    void writeToFile(const char* data, std::streamsize n)
    {
        if (!file_.is_open())
            return;

        file_.write(data, n);
        file_.flush();
        bytesWritten_ += static_cast<std::size_t>(n);

        if (bytesWritten_ >= maxBytes_)
            rotate();
    }

    // ── TeeBuf: duplicates output to both the original stream and the log file ──

    class TeeBuf : public std::streambuf
    {
    public:
        TeeBuf(std::streambuf* original, AppLogger& logger)
            : original_(original), logger_(logger) {}

    protected:
        int overflow(int c) override
        {
            if (c != EOF)
            {
                char ch = static_cast<char>(c);
                original_->sputc(ch);

                std::lock_guard<std::mutex> lock(logger_.mutex_);
                logger_.writeToFile(&ch, 1);
            }
            return c;
        }

        std::streamsize xsputn(const char* s, std::streamsize n) override
        {
            original_->sputn(s, n);

            std::lock_guard<std::mutex> lock(logger_.mutex_);
            logger_.writeToFile(s, n);
            return n;
        }

        int sync() override
        {
            original_->pubsync();
            return 0;
        }

    private:
        std::streambuf* original_;
        AppLogger& logger_;
    };

    std::mutex mutex_;
    std::ofstream file_;
    std::string path_;
    std::size_t maxBytes_{5 * 1024 * 1024};
    std::size_t bytesWritten_{0};
    int maxFiles_{3};

    std::unique_ptr<TeeBuf> coutTee_;
    std::unique_ptr<TeeBuf> cerrTee_;
    std::streambuf* origCout_{nullptr};
    std::streambuf* origCerr_{nullptr};
    bool installed_{false};
};
