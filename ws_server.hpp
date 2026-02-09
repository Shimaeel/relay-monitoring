#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <thread>
#include <memory>
#include <functional>
#include <string>
#include <iostream>

#include "ser_database.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

/**
 * @brief Convert SER records to JSON string
 */
inline std::string recordsToJSON(const std::vector<SERRecord>& records)
{
    std::string json = "[\n";
    
    int sno = 1;
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
        json += "    \"sno\": " + std::to_string(sno++) + ",\n";
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
 * @brief WebSocket session handler
 */
class WebSocketSession : public std::enable_shared_from_this<WebSocketSession>
{
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    SERDatabase& db_;

public:
    explicit WebSocketSession(tcp::socket&& socket, SERDatabase& db)
        : ws_(std::move(socket))
        , db_(db)
    {
    }

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
    void on_accept(beast::error_code ec)
    {
        if (ec)
        {
            std::cerr << "[WS] Accept error: " << ec.message() << "\n";
            return;
        }

        std::cout << "[WS] Client connected\n";
        
        // Send initial data
        sendData();
        
        // Start reading
        do_read();
    }

    void do_read()
    {
        ws_.async_read(
            buffer_,
            beast::bind_front_handler(&WebSocketSession::on_read, shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if (ec == websocket::error::closed)
        {
            std::cout << "[WS] Client disconnected\n";
            return;
        }

        if (ec)
        {
            std::cerr << "[WS] Read error: " << ec.message() << "\n";
            return;
        }

        // Handle incoming message
        std::string msg = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());

        std::cout << "[WS] Received: " << msg << "\n";

        if (msg == "refresh" || msg == "getData")
        {
            sendData();
        }

        // Continue reading
        do_read();
    }

    void sendData()
    {
        auto records = db_.getAllRecords();
        std::string json = recordsToJSON(records);
        
        ws_.text(true);
        ws_.async_write(
            net::buffer(json),
            beast::bind_front_handler(&WebSocketSession::on_write, shared_from_this()));
    }

    void on_write(beast::error_code ec, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if (ec)
        {
            std::cerr << "[WS] Write error: " << ec.message() << "\n";
            return;
        }
    }
};

/**
 * @brief WebSocket server listener
 */
class WebSocketListener : public std::enable_shared_from_this<WebSocketListener>
{
    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    SERDatabase& db_;

public:
    WebSocketListener(net::io_context& ioc, tcp::endpoint endpoint, SERDatabase& db)
        : ioc_(ioc)
        , acceptor_(ioc)
        , db_(db)
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

    void run()
    {
        do_accept();
    }

private:
    void do_accept()
    {
        acceptor_.async_accept(
            net::make_strand(ioc_),
            beast::bind_front_handler(&WebSocketListener::on_accept, shared_from_this()));
    }

    void on_accept(beast::error_code ec, tcp::socket socket)
    {
        if (ec)
        {
            std::cerr << "[WS] Accept error: " << ec.message() << "\n";
        }
        else
        {
            // Create the session and run it
            std::make_shared<WebSocketSession>(std::move(socket), db_)->run();
        }

        // Accept another connection
        do_accept();
    }
};

/**
 * @brief Simple WebSocket server wrapper
 */
class SERWebSocketServer
{
    net::io_context ioc_;
    std::shared_ptr<WebSocketListener> listener_;
    std::thread server_thread_;
    SERDatabase& db_;
    unsigned short port_;
    bool running_ = false;

public:
    explicit SERWebSocketServer(SERDatabase& db, unsigned short port = 8765)
        : db_(db)
        , port_(port)
    {
    }

    ~SERWebSocketServer()
    {
        stop();
    }

    bool start()
    {
        if (running_)
            return true;

        try
        {
            auto const address = net::ip::make_address("0.0.0.0");
            listener_ = std::make_shared<WebSocketListener>(ioc_, tcp::endpoint{address, port_}, db_);
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

    bool isRunning() const { return running_; }
};
