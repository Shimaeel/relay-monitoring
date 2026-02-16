// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file ws_db_server.hpp
 * @brief WebSocket server that exposes SQLite operations to JavaScript clients.
 *
 * @details Thin WebSocket transport layer that receives JSON requests, dispatches
 * them to the free functions in db_operations.hpp, and sends back the JSON
 * response.  All SQL logic lives in db_operations.hpp; all JSON parsing lives
 * in wsdb_json.hpp.  This file contains only networking (Boost.Beast / Asio).
 *
 * Optionally integrates with SharedDBMemory (shared_db_memory.hpp) to publish
 * change notifications on mutating actions (exec, define).
 *
 * ## Architecture
 *
 * ```
 *  Browser (db_client.js)
 *      |  WebSocket JSON
 *      v
 *  WSDBSession  ──dispatch──>  wsdb_ops::dbGetAll()
 *      |                       wsdb_ops::dbExec()
 *      v                       wsdb_ops::dbDefine()  ...
 *  WSDBListener  (TCP accept)
 *      |
 *  WSDBServer   (io_context + thread)
 * ```
 *
 * @see wsdb_json.hpp             JSON helpers
 * @see db_operations.hpp         SQLite operation functions
 * @see shared_db_memory.hpp      Shared-memory change notifications
 * @see ui/db_client.js           JavaScript client library
 *
 * @author Telnet-SML Development Team
 * @version 3.0.0
 * @date 2026
 */

#pragma once

#include "dll_export.hpp"
#include "wsdb_json.hpp"
#include "db_operations.hpp"
#include "shared_memory/shared_db_memory.hpp"

// ── Boost.Beast / Asio (WebSocket) ──
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/post.hpp>

#include <thread>
#include <memory>
#include <functional>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <mutex>
#include <sqlite3.h>

namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = boost::asio::ip::tcp;

// ────────────────────────────────────────────────────────────────────────────
//  WSDBSession  –  per-client WebSocket handler
// ────────────────────────────────────────────────────────────────────────────

class WSDBListener;

/**
 * @class WSDBSession
 * @brief Handles a single WebSocket client performing database operations.
 *
 * @details Receives JSON requests, dispatches to wsdb_ops:: free functions,
 * and sends the JSON response back.  All database access is serialised via
 * a shared mutex inside the wsdb_ops functions.
 */
class TELNET_SML_API WSDBSession : public std::enable_shared_from_this<WSDBSession>
{
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    sqlite3*         db_;
    std::mutex&      db_mutex_;
    SharedDBMemory*  shm_;          ///< Optional shared-memory change notifier

public:
    WSDBSession(tcp::socket&& socket, sqlite3* db, std::mutex& mtx,
                SharedDBMemory* shm = nullptr)
        : ws_(std::move(socket)), db_(db), db_mutex_(mtx), shm_(shm) {}

    void run()
    {
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        ws_.set_option(websocket::stream_base::decorator([](websocket::response_type& res) {
            res.set(http::field::server, "WSDB-Server");
        }));
        ws_.async_accept(beast::bind_front_handler(&WSDBSession::on_accept, shared_from_this()));
    }

private:
    void on_accept(beast::error_code ec)
    {
        if (ec) { std::cerr << "[WSDB] Accept error: " << ec.message() << "\n"; return; }
        std::cout << "[WSDB] Client connected\n";
        do_read();
    }

    void do_read()
    {
        ws_.async_read(buffer_, beast::bind_front_handler(&WSDBSession::on_read, shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t)
    {
        if (ec == websocket::error::closed) { std::cout << "[WSDB] Client disconnected\n"; return; }
        if (ec) { std::cerr << "[WSDB] Read error: " << ec.message() << "\n"; return; }

        std::string msg = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());

        std::string response = handleRequest(msg);

        ws_.text(true);
        auto buf = std::make_shared<std::string>(std::move(response));
        ws_.async_write(net::buffer(*buf),
            [self = shared_from_this(), buf](beast::error_code ec2, std::size_t) {
                if (ec2) std::cerr << "[WSDB] Write error: " << ec2.message() << "\n";
                else     self->do_read();
            });
    }

    // ── dispatcher — delegates to wsdb_ops:: free functions ───────────────
    std::string handleRequest(const std::string& json)
    {
        int64_t     id     = wsdb_json::getInt(json, "id");
        std::string action = wsdb_json::getString(json, "action");

        if (action == "getAll")
        {
            return wsdb_ops::dbGetAll(id, db_, db_mutex_,
                       wsdb_json::getString(json, "table"));
        }
        else if (action == "getIncremental")
        {
            return wsdb_ops::dbGetIncremental(id, db_, db_mutex_,
                       wsdb_json::getString(json, "table"),
                       wsdb_json::getInt(json, "sinceRowId", 0));
        }
        else if (action == "query")
        {
            return wsdb_ops::dbQuery(id, db_, db_mutex_,
                       wsdb_json::getString(json, "sql"),
                       wsdb_json::getStringArray(json, "params"));
        }
        else if (action == "exec")
        {
            std::string sql = wsdb_json::getString(json, "sql");
            auto params     = wsdb_json::getStringArray(json, "params");
            std::string result = wsdb_ops::dbExec(id, db_, db_mutex_, sql, params);
            notifyShm("exec", sql);
            return result;
        }
        else if (action == "tables")
        {
            return wsdb_ops::dbTables(id, db_, db_mutex_);
        }
        else if (action == "schema")
        {
            return wsdb_ops::dbSchema(id, db_, db_mutex_,
                       wsdb_json::getString(json, "table"));
        }
        else if (action == "define")
        {
            std::string table = wsdb_json::getString(json, "table");
            std::string result = wsdb_ops::dbDefine(id, db_, db_mutex_, table, json);
            notifyShm("define", table);
            return result;
        }

        std::ostringstream err;
        err << "{\"id\":" << id << ",\"ok\":false,\"error\":\"Unknown action: "
            << wsdb_json::escape(action) << "\"}";
        return err.str();
    }

    // ── shared-memory notification ────────────────────────────────────────
    void notifyShm(const std::string& action, const std::string& detail)
    {
        if (!shm_) return;
        std::ostringstream j;
        j << "{\"action\":\"" << wsdb_json::escape(action)
          << "\",\"detail\":\"" << wsdb_json::escape(detail) << "\"}";
        shm_->write(j.str());
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  WSDBListener  –  TCP accept loop
// ────────────────────────────────────────────────────────────────────────────

class TELNET_SML_API WSDBListener : public std::enable_shared_from_this<WSDBListener>
{
    net::io_context& ioc_;
    tcp::acceptor    acceptor_;
    sqlite3*         db_;
    std::mutex&      db_mutex_;
    SharedDBMemory*  shm_;

public:
    WSDBListener(net::io_context& ioc, tcp::endpoint ep, sqlite3* db, std::mutex& mtx,
                 SharedDBMemory* shm = nullptr)
        : ioc_(ioc), acceptor_(ioc), db_(db), db_mutex_(mtx), shm_(shm)
    {
        beast::error_code ec;
        acceptor_.open(ep.protocol(), ec);
        if (ec) { std::cerr << "[WSDB] Open: " << ec.message() << "\n"; return; }
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) { std::cerr << "[WSDB] Reuse: " << ec.message() << "\n"; return; }
        acceptor_.bind(ep, ec);
        if (ec) { std::cerr << "[WSDB] Bind: "  << ec.message() << "\n"; return; }
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) { std::cerr << "[WSDB] Listen: " << ec.message() << "\n"; return; }
    }

    void run() { do_accept(); }

private:
    void do_accept()
    {
        acceptor_.async_accept(net::make_strand(ioc_),
            beast::bind_front_handler(&WSDBListener::on_accept, shared_from_this()));
    }

    void on_accept(beast::error_code ec, tcp::socket socket)
    {
        if (!ec) std::make_shared<WSDBSession>(std::move(socket), db_, db_mutex_, shm_)->run();
        else     std::cerr << "[WSDB] Accept: " << ec.message() << "\n";
        do_accept();
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  WSDBServer  –  high-level start / stop wrapper
// ────────────────────────────────────────────────────────────────────────────

/**
 * @class WSDBServer
 * @brief High-level WebSocket server for generic SQLite access.
 *
 * Owns an `io_context` running in a dedicated thread.  Accepts a raw
 * `sqlite3*` handle so it can share the same database already opened
 * by `SERDatabase`.
 *
 * Optionally accepts a `SharedDBMemory*`; when provided, every mutating
 * action (exec, define) publishes a JSON notification to the shared
 * memory block so other processes can detect changes without polling.
 */
class TELNET_SML_API WSDBServer
{
    net::io_context                ioc_;
    std::shared_ptr<WSDBListener>  listener_;
    std::thread                    thread_;
    sqlite3*                       db_;
    std::mutex                     db_mutex_;
    SharedDBMemory*                shm_;
    unsigned short                 port_;
    bool                           running_ = false;

public:
    /**
     * @param db   Raw SQLite handle (lifetime managed externally).
     * @param port TCP port for the WebSocket server.
     * @param shm  Optional SharedDBMemory for change notifications.
     */
    explicit WSDBServer(sqlite3* db, unsigned short port = 8766,
                        SharedDBMemory* shm = nullptr)
        : db_(db), port_(port), shm_(shm) {}

    ~WSDBServer() { stop(); }

    WSDBServer(const WSDBServer&)            = delete;
    WSDBServer& operator=(const WSDBServer&) = delete;

    bool start()
    {
        if (running_) return true;
        try
        {
            auto addr = net::ip::make_address("0.0.0.0");
            listener_ = std::make_shared<WSDBListener>(ioc_, tcp::endpoint{addr, port_}, db_, db_mutex_, shm_);
            listener_->run();
            thread_ = std::thread([this]() { ioc_.run(); });
            running_ = true;
            std::cout << "[WSDB] Server started on ws://localhost:" << port_ << "\n";
            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "[WSDB] Start error: " << e.what() << "\n";
            return false;
        }
    }

    void stop()
    {
        if (!running_) return;
        ioc_.stop();
        if (thread_.joinable()) thread_.join();
        running_ = false;
        std::cout << "[WSDB] Server stopped\n";
    }

    bool isRunning() const { return running_; }
};
