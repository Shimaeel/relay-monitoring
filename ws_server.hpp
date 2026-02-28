// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file ws_server.hpp
 * @brief WebSocket Server for Real-time SER Data Access
 * 
 * @details This header implements a WebSocket server using Boost.Beast for
 * providing real-time access to System Event Records from the web UI.
 * 
 * ## Architecture Overview
 * 
 * @dot
 * digraph WSArchitecture {
 *     rankdir=TB;
 *     node [shape=box, style=filled, fillcolor=lightyellow];
 *     
 *     subgraph cluster_server {
 *         label="WebSocket Server";
 *         WS [label="SERWebSocketServer\n(Main Controller)"];
 *         Listener [label="WebSocketListener\n(Accept Loop)"];
 *         Session [label="WebSocketSession\n(Per Client)"];
 *     }
 *     
 *     subgraph cluster_deps {
 *         label="Dependencies";
 *         DB [label="SERDatabase"];
 *         IOC [label="io_context"];
 *     }
 *     
 *     subgraph cluster_external {
 *         label="External";
 *         Browser [label="Web Browser\n(index.html)", shape=ellipse, fillcolor=lightblue];
 *     }
 *     
 *     WS -> Listener [label="owns"];
 *     WS -> IOC [label="owns"];
 *     Listener -> Session [label="creates"];
 *     Session -> DB [label="queries"];
 *     Browser -> Session [label="WebSocket\nws://localhost:8765"];
 * }
 * @enddot
 * 
 * ## Communication Protocol
 * 
 * @msc
 * Browser,Session,Database;
 * Browser->Session [label="connect"];
 * Session->Database [label="getAllRecords()"];
 * Database->Session [label="records"];
 * Session->Browser [label="ASN.1 BER/TLV"];
 * Browser->Session [label="\"refresh\""];
 * Session->Database [label="getAllRecords()"];
 * Database->Session [label="records"];
 * Session->Browser [label="ASN.1 BER/TLV"];
 * Browser->Session [label="close"];
 * @endmsc
 * 
 * ## Browser Architecture (Client Side)
 * 
 * ```
 * WebSocket Client
 *      ↓
 * JS Worker (Optional)
 *      ↓
 * Main JS Thread (DOM Access)
 *      ↓
 * Tabulator / JSON Export
 * ```
 * 
 * ## ASN.1 BER/TLV Payload Format
 * 
 * Top-level TLV:
 * - Tag 0x61 (APPLICATION 1, constructed)
 * - Value: zero or more Record TLVs
 * 
 * Record TLV:
 * - Tag 0x30 (SEQUENCE, constructed)
 * - Value: context-specific primitive fields
 *   - 0x80: record_id (string)
 *   - 0x81: timestamp (string)
 *   - 0x82: status (string)
 *   - 0x83: description (string)
 * 
 * ## Usage Example
 * 
 * @code{.cpp}
 * SERDatabase db("records.db");
 * db.open();
 * 
 * SERWebSocketServer wsServer(db, 8765);
 * wsServer.start();
 * // Server now listening on ws://localhost:8765
 * 
 * // ... application runs ...
 * 
 * wsServer.stop();
 * @endcode
 * 
 * @see SERDatabase Database for record storage
 * @see asn_tlv::encodeSerRecordsToTlv() ASN.1 BER/TLV conversion function
 * @see index.html Web UI that connects to this server
 * 
 * @note Uses Boost.Beast for WebSocket implementation
 * @note Server runs in separate thread from main application
 * @note Supports multiple concurrent client connections
 * 
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include "dll_export.hpp"
#include <optional>
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
#include <deque>
#include <iostream>
#include <set>
#include <mutex>

#include "asn_tlv_codec.hpp"
#include "ser_database.hpp"

// Forward declaration for session manager
class WebSocketSession;
class SessionManager;

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

/**
 * @brief Convert vector of SER records to JSON string
 * 
 * @details Formats SER records as JSON array compatible with web UI.
 * Handles timestamp parsing and special character escaping.
 * 
 * @param records Vector of SERRecord to convert
 * 
 * @return std::string JSON array string
 * 
 * @note Splits timestamp into separate date and time fields
 * @note Escapes quotes and backslashes in element description
 * 
 * @see WebSocketSession::sendData() Uses this for client responses
 */
inline std::string recordsToJSON(const std::vector<SERRecord>& records)
{
    std::string json = "[\n";
    
    for (size_t i = 0; i < records.size(); ++i)
    {
        const auto& rec = records[i];
        
        // Parse timestamp to extract date and time
        std::string date = rec.timestamp;
        std::string time = "";
        
        size_t spacePos = rec.timestamp.find(' ');
        if (spacePos != std::string::npos)
        {
            date = rec.timestamp.substr(0, spacePos);
            time = rec.timestamp.substr(spacePos + 1);
        }
        
        // Escape special characters in description
        std::string element = rec.description;
        for (size_t j = 0; j < element.length(); ++j)
        {
            if (element[j] == '"' || element[j] == '\\')
            {
                element.insert(j, "\\");
                ++j;
            }
        }
        
        json += "  {\n";
        json += "    \"sno\": " + rec.record_id + ",\n";  // Use original # from relay
        json += "    \"date\": \"" + date + "\",\n";
        json += "    \"time\": \"" + time + "\",\n";
        json += "    \"element\": \"" + element + "\",\n";
        json += "    \"state\": \"" + rec.status + "\"\n";
        json += "  }";
        
        if (i < records.size() - 1)
            json += ",";
        
        json += "\n";
    }
    
    json += "]";
    return json;
}

/**
 * @class SessionManager
 * @brief Manages all active WebSocket sessions for broadcast
 * 
 * @details Thread-safe container for tracking connected WebSocket sessions.
 * Enables push-based data delivery to all connected clients simultaneously.
 * 
 * ## Broadcast Flow
 * 
 * @dot
 * digraph Broadcast {
 *     rankdir=LR;
 *     node [shape=box, style=filled];
 *     
 *     FSM [label="FSM\n(after processing)", fillcolor=lightyellow];
 *     Manager [label="SessionManager", fillcolor=lightgreen];
 *     S1 [label="Session 1"];
 *     S2 [label="Session 2"];
 *     S3 [label="Session N"];
 *     
 *     FSM -> Manager [label="broadcast()"];
 *     Manager -> S1;
 *     Manager -> S2;
 *     Manager -> S3;
 * }
 * @enddot
 */
class TELNET_SML_API SessionManager
{
public:
    /**
     * @brief Register a new session for broadcast
     * @param session Shared pointer to the WebSocket session
     */
    void add(std::shared_ptr<WebSocketSession> session)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.insert(session);
    }

    /**
     * @brief Unregister a session (on disconnect)
     * @param session Shared pointer to the WebSocket session
     */
    void remove(std::shared_ptr<WebSocketSession> session)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(session);
    }

    /**
     * @brief Get all currently connected sessions
     * @return Vector of shared pointers to active sessions
     */
    std::vector<std::shared_ptr<WebSocketSession>> getSessions() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return {sessions_.begin(), sessions_.end()};
    }

    /**
     * @brief Get number of connected clients
     */
    std::size_t count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return sessions_.size();
    }

private:
    mutable std::mutex mutex_;
    std::set<std::shared_ptr<WebSocketSession>> sessions_;
};


/**
 * @class WebSocketSession
 * @brief Handles a single WebSocket client connection
 * 
 * @details Manages the lifecycle of a WebSocket session:
 * - Accepts WebSocket handshake
 * - Sends initial data upon connection
 * - Listens for client messages (e.g., "refresh")
 * - Responds with current database records as ASN.1 BER/TLV
 * 
 * Uses shared_from_this() pattern for safe async callback handling.
 * 
 * ## Session Lifecycle
 * 
 * @dot
 * digraph SessionState {
 *     rankdir=LR;
 *     node [shape=ellipse, style=filled];
 *     
 *     Created [fillcolor=lightgray];
 *     Accepting [fillcolor=lightyellow];
 *     Connected [fillcolor=lightgreen];
 *     Reading [fillcolor=lightblue];
 *     Closed [fillcolor=lightcoral];
 *     
 *     Created -> Accepting [label="run()"];
 *     Accepting -> Connected [label="handshake"];
 *     Accepting -> Closed [label="error"];
 *     Connected -> Reading [label="do_read()"];
 *     Reading -> Reading [label="message"];
 *     Reading -> Closed [label="close/error"];
 * }
 * @enddot
 */
class TELNET_SML_API WebSocketSession : public std::enable_shared_from_this<WebSocketSession>
{
public:
    /// Callback type for relay command forwarding:
    ///   input  = command string (e.g. "FIL DIR")
    ///   output = relay response text
    using CommandHandler = std::function<std::string(const std::string&)>;

    /// Callback type for JSON action handling (time sync etc.):
    ///   input  = action string (e.g. "read_time", "sync_time")
    ///   output = JSON response string
    using ActionHandler = std::function<std::string(const std::string&)>;

private:
    websocket::stream<beast::tcp_stream> ws_;   ///< WebSocket stream over TCP
    beast::flat_buffer buffer_;                  ///< Buffer for incoming messages
    SERDatabase& db_;                            ///< Reference to database for queries
    SessionManager* sessionMgr_;                 ///< Session manager for registration
    bool writing_ = false;                       ///< Flag to prevent concurrent writes
    bool pending_read_ = false;                  ///< Flag for pending read after write
    std::vector<uint8_t> write_buffer_;          ///< Buffer to hold data during async write
    std::vector<uint8_t> broadcast_buffer_;      ///< Buffer for broadcast data
    std::string text_write_buffer_;              ///< Buffer for text responses (FIL DIR etc.)
    CommandHandler cmdHandler_;                   ///< Optional handler for relay commands
    ActionHandler actionHandler_;                 ///< Optional handler for JSON actions (time sync)

    // ── Write queue for guaranteed delivery ─────────────────────────────
    /// Pending message: binary payload or text string, with mode flag
    struct PendingMsg
    {
        enum Kind { Binary, Text, SendAll } kind;
        std::vector<uint8_t> binary_data;
        std::string          text_data;
    };
    std::deque<PendingMsg> write_queue_;  ///< Queued writes while a write is in-flight

public:
    /**
     * @brief Construct WebSocket session from accepted socket
     * 
     * @param socket TCP socket from accepted connection (moved)
     * @param db Reference to SER database for data access
     * @param sessionMgr Pointer to session manager (may be null for legacy mode)
     * @param cmdHandler Optional callback for relay command forwarding
     * @param actionHandler Optional callback for JSON action handling (time sync)
     */
    explicit WebSocketSession(tcp::socket&& socket, SERDatabase& db,
                              SessionManager* sessionMgr = nullptr,
                              CommandHandler cmdHandler = nullptr,
                              ActionHandler actionHandler = nullptr)
        : ws_(std::move(socket))
        , db_(db)
        , sessionMgr_(sessionMgr)
        , cmdHandler_(std::move(cmdHandler))
        , actionHandler_(std::move(actionHandler))
    {
    }
    
    /**
     * @brief Destructor - unregister from session manager
     */
    ~WebSocketSession()
    {
        // Note: Cannot use shared_from_this() in destructor
        // Deregistration is handled in on_read when connection closes
    }

    /**
     * @brief Send data to this client (for broadcast push)
     * 
     * @details Called by SessionManager to push data to connected clients.
     * Thread-safe via strand posting.
     * 
     * @param data Binary data to send (ASN.1 BER/TLV encoded)
     */
    void sendBroadcast(const std::vector<uint8_t>& data)
    {
        // Post to strand to ensure thread safety
        net::post(ws_.get_executor(), [self = shared_from_this(), data]() {
            self->do_broadcast(data);
        });
    }

    /**
     * @brief Send text data to this client (for non-SER command responses)
     * 
     * @details Called by SERWebSocketServer::broadcastText() to push
     * raw relay text responses (e.g. FIL DIR) to connected clients.
     * Thread-safe via strand posting.
     * 
     * @param text Text data to send
     */
    void sendTextBroadcast(const std::string& text)
    {
        net::post(ws_.get_executor(), [self = shared_from_this(), text]() {
            self->sendTextResponse(text);
        });
    }

    /**
     * @brief Start the session by accepting WebSocket handshake
     * 
     * @details Configures timeout and server decorator, then initiates
     * async WebSocket handshake acceptance.
     */
    void run()
    {
        // Set suggested timeout settings for the websocket
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

        // Set a decorator to change the Server of the handshake
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res) {
                res.set(http::field::server, "SER-WebSocket-Server");
            }));

        // Accept the websocket handshake
        ws_.async_accept(
            beast::bind_front_handler(&WebSocketSession::on_accept, shared_from_this()));
    }

private:
    /**
     * @brief Handle WebSocket handshake completion
     * 
     * @param ec Error code from handshake (empty on success)
     */
    void on_accept(beast::error_code ec)
    {
        if (ec)
        {
            std::cerr << "[WS] Accept error: " << ec.message() << "\n";
            return;
        }

        std::cout << "[WS] Client connected\n";
        
        // Register with session manager for broadcast
        if (sessionMgr_)
            sessionMgr_->add(shared_from_this());
        
        // Send initial data, then start reading when write completes
        pending_read_ = true;
        sendData();
    }

    /**
     * @brief Perform broadcast write operation (called on strand)
     *
     * @details If a write is already in progress, the payload is queued.
     * The queue is drained automatically when the current write completes.
     */
    void do_broadcast(const std::vector<uint8_t>& data)
    {
        if (writing_)
        {
            // Queue for delivery once the in-flight write finishes
            write_queue_.push_back({PendingMsg::Binary, data, {}});
            return;
        }
        
        writing_ = true;
        broadcast_buffer_ = data;
        
        ws_.binary(true);
        ws_.async_write(
            net::buffer(broadcast_buffer_),
            beast::bind_front_handler(&WebSocketSession::on_broadcast_write, shared_from_this()));
    }
    
    /**
     * @brief Handle broadcast write completion — drains pending queue.
     */
    void on_broadcast_write(beast::error_code ec, std::size_t /*bytes_transferred*/)
    {
        writing_ = false;
        
        if (ec)
        {
            std::cerr << "[WS] Broadcast write error: " << ec.message() << "\n";
            write_queue_.clear();
            return;
        }

        drainQueue();
    }

    /**
     * @brief Send the next queued message (if any).
     *
     * @details Called after every successful write to flush pending data.
     */
    void drainQueue()
    {
        if (writing_ || write_queue_.empty())
            return;

        auto msg = std::move(write_queue_.front());
        write_queue_.pop_front();

        switch (msg.kind)
        {
        case PendingMsg::Binary:
            writing_ = true;
            broadcast_buffer_ = std::move(msg.binary_data);
            ws_.binary(true);
            ws_.async_write(
                net::buffer(broadcast_buffer_),
                beast::bind_front_handler(&WebSocketSession::on_broadcast_write,
                                          shared_from_this()));
            break;

        case PendingMsg::Text:
            writing_ = true;
            text_write_buffer_ = std::move(msg.text_data);
            ws_.text(true);
            ws_.async_write(
                net::buffer(text_write_buffer_),
                beast::bind_front_handler(&WebSocketSession::on_broadcast_write,
                                          shared_from_this()));
            break;

        case PendingMsg::SendAll:
            sendData();   // will set writing_ itself
            break;
        }
    }

    /**
     * @brief Initiate async read for next client message
     */
    void do_read()
    {
        ws_.async_read(
            buffer_,
            beast::bind_front_handler(&WebSocketSession::on_read, shared_from_this()));
    }

    /**
     * @brief Handle received client message
     * 
     * @param ec Error code from read operation
     * @param bytes_transferred Number of bytes received
     */
    void on_read(beast::error_code ec, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if (ec == websocket::error::closed)
        {
            std::cout << "[WS] Client disconnected\n";
            // Deregister from session manager
            if (sessionMgr_)
                sessionMgr_->remove(shared_from_this());
            return;
        }

        if (ec)
        {
            std::cerr << "[WS] Read error: " << ec.message() << "\n";
            // Deregister from session manager on error too
            if (sessionMgr_)
                sessionMgr_->remove(shared_from_this());
            return;
        }

        // Handle incoming message
        std::string msg = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());

        // Mask sensitive payloads (passwords) in log output
        if (isJsonAction(msg) && extractAction(msg) == "change_password")
            std::cout << "[WS] Received: {\"action\":\"change_password\",...}\n";
        else
            std::cout << "[WS] Received: " << msg << "\n";

        if (msg == "refresh" || msg == "getData")
        {
            // Send data, then continue reading when write completes
            pending_read_ = true;
            sendData();
        }
        else if (actionHandler_ && isJsonAction(msg))
        {
            // JSON action (e.g. read_time, sync_time, change_password) — handle asynchronously
            std::string action = extractAction(msg);
            if (action == "change_password")
                std::cout << "[WS] JSON action detected: " << action << " (payload masked)\n";
            else
                std::cout << "[WS] JSON action detected: " << action << "\n";

            // Run handler on a detached thread to avoid blocking the strand
            // Pass full JSON message so handlers can extract extra fields
            auto self = shared_from_this();
            auto handler = actionHandler_;
            auto fullMsg = msg;
            std::thread([self, handler, fullMsg]() {
                std::string response = handler(fullMsg);
                // Post text response back onto the session's strand
                net::post(self->ws_.get_executor(), [self, response]() {
                    self->sendTextResponse(response);
                    self->do_read();
                });
            }).detach();
        }
        else if (cmdHandler_)
        {
            // Queue command to ReceptionWorker (async — response comes via broadcastText)
            std::cout << "[WS] Queuing command via handler: " << msg << "\n";
            cmdHandler_(msg);
            do_read();
        }
        else
        {
            // No handler registered, ignore
            do_read();
        }
    }

    /**
     * @brief Send current database records to client as ASN.1 BER/TLV.
     * 
     * @details Queries all records from database and sends as binary.
     * If a write is in-flight the request is queued and sent later.
     */
    void sendData()
    {
        if (writing_)
        {
            // Queue a full-DB send for when the current write finishes
            write_queue_.push_back({PendingMsg::SendAll, {}, {}});
            return;
        }
        
        writing_ = true;
        
        auto records = db_.getAllRecords();
        write_buffer_ = asn_tlv::encodeSerRecordsToTlv(records);  // Store in member to keep alive

        ws_.binary(true);
        ws_.async_write(
            net::buffer(write_buffer_),  // Use member buffer
            beast::bind_front_handler(&WebSocketSession::on_write, shared_from_this()));
    }

    /**
     * @brief Handle write completion
     * 
     * @param ec Error code from write operation
     * @param bytes_transferred Number of bytes written
     */
    void on_write(beast::error_code ec, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);
        
        writing_ = false;

        if (ec)
        {
            std::cerr << "[WS] Write error: " << ec.message() << "\n";
            write_queue_.clear();
            return;
        }
        
        // Start reading if a read was pending
        if (pending_read_)
        {
            pending_read_ = false;
            do_read();
        }

        // Flush any queued writes (broadcasts / text / sendAll)
        drainQueue();
    }

    /**
     * @brief Check if a message is a JSON action (contains "action" key).
     */
    static bool isJsonAction(const std::string& msg)
    {
        // Quick heuristic: starts with '{' and contains "action"
        if (msg.empty() || msg[0] != '{') return false;
        return msg.find("\"action\"") != std::string::npos;
    }

    /**
     * @brief Extract the "action" value from a simple JSON object.
     *
     * Lightweight parser — no dependency on a JSON library.
     */
    static std::string extractAction(const std::string& json)
    {
        const std::string key = "\"action\"";
        auto pos = json.find(key);
        if (pos == std::string::npos) return {};
        pos = json.find(':', pos + key.size());
        if (pos == std::string::npos) return {};
        pos = json.find('"', pos + 1);
        if (pos == std::string::npos) return {};
        auto end = json.find('"', pos + 1);
        if (end == std::string::npos) return {};
        return json.substr(pos + 1, end - pos - 1);
    }

    /**
     * @brief Send a plain text response to this client (for FIL DIR etc.)
     * 
     * @param text The text response string to send
     */
    void sendTextResponse(const std::string& text)
    {
        if (writing_)
        {
            // Queue for delivery once the in-flight write finishes
            write_queue_.push_back({PendingMsg::Text, {}, text});
            return;
        }

        writing_ = true;
        text_write_buffer_ = text;

        ws_.text(true);  // Send as text frame (not binary)
        ws_.async_write(
            net::buffer(text_write_buffer_),
            beast::bind_front_handler(&WebSocketSession::on_write, shared_from_this()));
    }
};

/**
 * @class WebSocketListener
 * @brief Listens for and accepts incoming WebSocket connections
 * 
 * @details Manages the TCP acceptor and creates WebSocketSession instances
 * for each incoming connection. Runs continuously accepting connections.
 * 
 * ## Accept Loop
 * 
 * @dot
 * digraph AcceptLoop {
 *     rankdir=TB;
 *     node [shape=box];
 *     
 *     start [shape=ellipse, label="run()"];
 *     accept [label="async_accept()"];
 *     create [label="Create Session"];
 *     run [label="session->run()"];
 *     
 *     start -> accept;
 *     accept -> create [label="connection"];
 *     create -> run;
 *     run -> accept [label="loop"];
 * }
 * @enddot
 */
class TELNET_SML_API WebSocketListener : public std::enable_shared_from_this<WebSocketListener>
{
    net::io_context& ioc_;       ///< Reference to I/O context
    tcp::acceptor acceptor_;      ///< TCP acceptor for incoming connections
    SERDatabase& db_;             ///< Reference to database for sessions
    SessionManager* sessionMgr_;  ///< Session manager for broadcast support
    WebSocketSession::CommandHandler cmdHandler_;  ///< Command handler for relay forwarding
    WebSocketSession::ActionHandler actionHandler_;  ///< Action handler for JSON actions (time sync)

public:
    /**
     * @brief Construct listener on specified endpoint
     * 
     * @param ioc I/O context for async operations
     * @param endpoint TCP endpoint to listen on (address + port)
     * @param db Database reference to pass to sessions
     * @param sessionMgr Session manager for broadcast (may be null)
     * @param cmdHandler Command handler for relay forwarding (may be null)
     * @param actionHandler Action handler for JSON actions like time sync (may be null)
     */
    WebSocketListener(net::io_context& ioc, tcp::endpoint endpoint, SERDatabase& db,
                      SessionManager* sessionMgr = nullptr,
                      WebSocketSession::CommandHandler cmdHandler = nullptr,
                      WebSocketSession::ActionHandler actionHandler = nullptr)
        : ioc_(ioc)
        , acceptor_(ioc)
        , db_(db)
        , sessionMgr_(sessionMgr)
        , cmdHandler_(std::move(cmdHandler))
        , actionHandler_(std::move(actionHandler))
    {
        beast::error_code ec;

        // Open the acceptor
        acceptor_.open(endpoint.protocol(), ec);
        if (ec)
        {
            std::cerr << "[WS] Open error: " << ec.message() << "\n";
            return;
        }

        // Allow address reuse
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec)
        {
            std::cerr << "[WS] Set option error: " << ec.message() << "\n";
            return;
        }

        // Bind to the server address
        acceptor_.bind(endpoint, ec);
        if (ec)
        {
            std::cerr << "[WS] Bind error: " << ec.message() << "\n";
            return;
        }

        // Start listening for connections
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec)
        {
            std::cerr << "[WS] Listen error: " << ec.message() << "\n";
            return;
        }
    }

    /**
     * @brief Start the accept loop
     * 
     * @details Begins accepting incoming connections. Called after constructor.
     */
    void run()
    {
        do_accept();
    }

private:
    /**
     * @brief Initiate async accept for next connection
     */
    void do_accept()
    {
        acceptor_.async_accept(
            net::make_strand(ioc_),
            beast::bind_front_handler(&WebSocketListener::on_accept, shared_from_this()));
    }

    /**
     * @brief Handle accepted connection
     * 
     * @param ec Error code from accept operation
     * @param socket Accepted TCP socket
     */
    void on_accept(beast::error_code ec, tcp::socket socket)
    {
        if (ec)
        {
            std::cerr << "[WS] Accept error: " << ec.message() << "\n";
        }
        else
        {
            // Create the session and run it (with session manager + command handler + action handler)
            std::make_shared<WebSocketSession>(std::move(socket), db_, sessionMgr_, cmdHandler_, actionHandler_)->run();
        }

        // Accept another connection
        do_accept();
    }
};

/**
 * @class SERWebSocketServer
 * @brief High-level WebSocket server wrapper for SER data access
 * 
 * @details Provides simple start/stop interface for the WebSocket server.
 * Manages I/O context, listener, and server thread. Used by main application
 * to expose SER data to the web UI.
 * 
 * ## Server Lifecycle
 * 
 * @dot
 * digraph ServerLifecycle {
 *     rankdir=LR;
 *     node [shape=ellipse, style=filled];
 *     
 *     Stopped [fillcolor=lightgray];
 *     Running [fillcolor=lightgreen];
 *     
 *     Stopped -> Running [label="start()"];
 *     Running -> Stopped [label="stop()"];
 *     Running -> Stopped [label="destructor"];
 * }
 * @enddot
 * 
 * ## Thread Model
 * 
 * The server runs I/O context in a separate thread from the main application:
 * 
 * @dot
 * digraph ThreadModel {
 *     rankdir=TB;
 *     node [shape=box, style=filled, fillcolor=lightyellow];
 *     
 *     main [label="Main Thread\n(FSM, Telnet)"];
 *     server [label="Server Thread\n(WebSocket I/O)"];
 *     db [label="SERDatabase\n(Shared)", shape=cylinder, fillcolor=lightblue];
 *     
 *     main -> db [label="write"];
 *     server -> db [label="read"];
 * }
 * @enddot
 */
class TELNET_SML_API SERWebSocketServer
{
    net::io_context ioc_;                          ///< I/O context for async operations
    std::shared_ptr<WebSocketListener> listener_;  ///< Connection listener
    std::thread server_thread_;                    ///< Thread running I/O context
    SERDatabase& db_;                              ///< Reference to SER database
    unsigned short port_;                          ///< Port number to listen on
    bool running_ = false;                         ///< Server running state flag
    SessionManager sessionMgr_;                    ///< Session manager for broadcast
    WebSocketSession::CommandHandler cmdHandler_;  ///< Command handler for relay forwarding
    WebSocketSession::ActionHandler actionHandler_;  ///< Action handler for JSON actions (time sync)

public:
    /**
     * @brief Construct WebSocket server for given database
     * 
     * @param db Reference to SER database (must outlive server)
     * @param port Port number to listen on (default: 8765)
     * 
     * @post running_ == false (call start() to begin)
     */
    explicit SERWebSocketServer(SERDatabase& db, unsigned short port = 8765)
        : db_(db)
        , port_(port)
    {
    }

    /**
     * @brief Destructor - stops server if running
     */
    ~SERWebSocketServer()
    {
        stop();
    }

    /**
     * @brief Set command handler for forwarding relay commands (e.g. FIL DIR)
     * 
     * @details Must be called before start(). The handler receives a command
     * string and returns the relay's text response.
     * 
     * @param handler Callback: string command → string response
     */
    void setCommandHandler(WebSocketSession::CommandHandler handler)
    {
        cmdHandler_ = std::move(handler);
    }

    /**
     * @brief Set action handler for JSON actions (e.g. read_time, sync_time)
     * 
     * @details Must be called before start(). The handler receives an action
     * string and returns a JSON response string.
     * 
     * @param handler Callback: string action → string JSON response
     */
    void setActionHandler(WebSocketSession::ActionHandler handler)
    {
        actionHandler_ = std::move(handler);
    }

    /**
     * @brief Start the WebSocket server
     * 
     * @details Creates listener and starts I/O thread. Safe to call if already running.
     * 
     * @return true Server started successfully
     * @return false Failed to start (port in use, etc.)
     * 
     * @post On success: isRunning() == true
     */
    bool start()
    {
        if (running_)
            return true;

        try
        {
            auto const address = net::ip::make_address("0.0.0.0");
            listener_ = std::make_shared<WebSocketListener>(ioc_, tcp::endpoint{address, port_}, db_, &sessionMgr_, cmdHandler_, actionHandler_);
            listener_->run();

            server_thread_ = std::thread([this]() {
                ioc_.run();
            });

            running_ = true;
            std::cout << "[WS] Server started on ws://localhost:" << port_ << "\n";
            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "[WS] Start error: " << e.what() << "\n";
            return false;
        }
    }

    /**
     * @brief Stop the WebSocket server
     * 
     * @details Stops I/O context and joins server thread. Safe to call if not running.
     * 
     * @post isRunning() == false
     */
    void stop()
    {
        if (!running_)
            return;

        ioc_.stop();
        if (server_thread_.joinable())
            server_thread_.join();
        
        running_ = false;
        std::cout << "[WS] Server stopped\n";
    }

    /**
     * @brief Check if server is currently running
     * 
     * @return true Server is accepting connections
     * @return false Server is stopped
     */
    bool isRunning() const { return running_; }
    
    /**
     * @brief Broadcast data to all connected clients (push model)
     * 
     * @details Called after FSM processing to push new data to all browsers.
     * This enables real-time updates without polling.
     * 
     * @param records Vector of SER records to broadcast
     */
    void broadcast(const std::vector<SERRecord>& records)
    {
        if (!running_)
            return;
        
        auto payload = asn_tlv::encodeSerRecordsToTlv(records);
        if (payload.empty())
            return;
        
        auto sessions = sessionMgr_.getSessions();
        std::cout << "[WS] Broadcasting to " << sessions.size() << " clients\n";
        
        for (auto& session : sessions)
        {
            if (session)
                session->sendBroadcast(payload);
        }
    }

    /**
     * @brief Broadcast all SER records from the database to all clients.
     *
     * @details Useful after a SER poll to refresh the full table in the UI.
     */
    void broadcastAll()
    {
        if (!running_)
            return;

        auto records = db_.getAllRecords();
        auto payload = asn_tlv::encodeSerRecordsToTlv(records);
        if (payload.empty())
            return;

        auto sessions = sessionMgr_.getSessions();
        std::cout << "[WS] Broadcasting full DB to " << sessions.size() << " clients\n";

        for (auto& session : sessions)
        {
            if (session)
                session->sendBroadcast(payload);
        }
    }
    
    /**
     * @brief Broadcast raw binary payload to all connected clients
     * 
     * @param payload Pre-encoded ASN.1 BER/TLV data
     */
    void broadcastRaw(const std::vector<uint8_t>& payload)
    {
        if (!running_ || payload.empty())
            return;
        
        auto sessions = sessionMgr_.getSessions();
        for (auto& session : sessions)
        {
            if (session)
                session->sendBroadcast(payload);
        }
    }
    
    /**
     * @brief Get number of connected clients
     */
    std::size_t clientCount() const
    {
        return sessionMgr_.count();
    }

    /**
     * @brief Broadcast text response to all connected clients
     * 
     * @details Used for non-SER relay command responses (e.g. FIL DIR).
     * Sends as text WebSocket frame so JS can read directly.
     * 
     * @param text Raw relay response text
     */
    void broadcastText(const std::string& text)
    {
        if (!running_ || text.empty())
            return;
        
        auto sessions = sessionMgr_.getSessions();
        std::cout << "[WS] Broadcasting text to " << sessions.size() << " clients (" << text.size() << " bytes)\n";
        
        for (auto& session : sessions)
        {
            if (session)
                session->sendTextBroadcast(text);
        }
    }
};
