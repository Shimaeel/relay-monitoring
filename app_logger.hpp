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
     * @brief Open the log file and redirect stdout/stderr through it.
     *
     * @details Opens @p filePath in append mode so restart-and-continue
     * scenarios don't clobber prior output, then installs a custom
     * @c streambuf (TeeBuf) on @c std::cout and @c std::cerr. Existing
     * code that writes via those streams works unchanged — every byte is
     * duplicated into the log file while still appearing on the console.
     *
     * Safe to call multiple times; the second call reconfigures the
     * limits but will leak the previously-installed tee buffers, so it is
     * intended to run once at application startup.
     *
     * @param filePath  Log file path (e.g. @c "app.log").
     * @param maxBytes  Maximum bytes before the file is rotated to
     *                  @c app.log.1, @c app.log.2, ... Defaults to 5 MB.
     * @param maxFiles  Number of rotated files to retain. Defaults to 3,
     *                  giving ~15 MB of history at the default size.
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
     * @brief Detach the tees, restore the original streams, and close the file.
     *
     * @details Restores the @c streambuf that @c std::cout and @c std::cerr
     * used before init() and destroys the tee buffers, then closes the log
     * file. Idempotent — calling shutdown() a second time is a no-op. Must
     * be invoked before process exit, otherwise the stream tees outlive the
     * log file they reference.
     *
     * @post @c std::cout and @c std::cerr point at their original
     *       streambufs; subsequent output is not captured to the log file.
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

    /**
     * @brief Access the process-wide AppLogger singleton.
     *
     * @details Uses Meyers' singleton to lazily construct a single instance
     * that lives for the duration of the program. Thread-safe per the C++11
     * magic-statics rule.
     */
    static AppLogger& instance()
    {
        static AppLogger inst;
        return inst;
    }

    /**
     * @brief Rotate the current log file into the numbered ring.
     *
     * @details Renames the chain @c app.log.(N-1) → @c app.log.N down to
     * @c app.log → @c app.log.1, removing the oldest file to keep the ring
     * bounded at @c maxFiles_. Reopens the primary file in truncation mode
     * and resets the byte counter so the next write starts from zero.
     *
     * @pre The caller must hold @c mutex_ (invoked from writeToFile()).
     */
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

    /**
     * @brief Append bytes to the log file and rotate if the size cap is hit.
     *
     * @details Flushes after every write so a crash does not lose the tail
     * of the log. Updates the running byte counter and, when it crosses
     * @c maxBytes_, invokes rotate() to start a fresh file.
     *
     * @param data  Pointer to the bytes to write.
     * @param n     Number of bytes available at @p data.
     *
     * @pre The caller must hold @c mutex_; the tee's overflow() and
     *      xsputn() both lock before delegating here.
     */
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

    /**
     * @class TeeBuf
     * @brief `std::streambuf` that forwards every write to a target buffer
     *        and the owning AppLogger.
     *
     * @details Installed on @c std::cout and @c std::cerr by init(). Every
     * character and block written to those streams is forwarded verbatim
     * to the original @c streambuf (so console output is unaffected) and
     * simultaneously appended to the AppLogger's file via writeToFile().
     * The file write is serialised through AppLogger::mutex_ so multi-
     * threaded output does not interleave inside a single log record.
     */
    class TeeBuf : public std::streambuf
    {
    public:
        /**
         * @brief Wrap an existing streambuf and route writes through it.
         *
         * @param original  The streambuf that the tee should delegate to;
         *                  typically @c std::cout.rdbuf() or
         *                  @c std::cerr.rdbuf() captured before
         *                  installation. Must outlive this TeeBuf.
         * @param logger    AppLogger instance that owns the file sink.
         */
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

    std::mutex mutex_;                             ///< Guards @c file_ and rotation.
    std::ofstream file_;                           ///< Currently-open log file.
    std::string path_;                             ///< Path to the primary log file.
    std::size_t maxBytes_{5 * 1024 * 1024};        ///< Rotation threshold in bytes.
    std::size_t bytesWritten_{0};                  ///< Bytes written since last rotation.
    int maxFiles_{3};                              ///< Number of rotated files to keep.

    std::unique_ptr<TeeBuf> coutTee_;              ///< Tee buffer installed on @c std::cout.
    std::unique_ptr<TeeBuf> cerrTee_;              ///< Tee buffer installed on @c std::cerr.
    std::streambuf* origCout_{nullptr};            ///< Saved original @c std::cout streambuf.
    std::streambuf* origCerr_{nullptr};            ///< Saved original @c std::cerr streambuf.
    bool installed_{false};                        ///< True while the tees are in place.
};
