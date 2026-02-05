#include "client.hpp"

#include <iostream>
#include <thread>
#include <fstream>
#include <sstream>
#include <vector>
#include <chrono>
#include <ctime>

// ================= CONSTRUCTOR =================
TelnetClient::TelnetClient()
    : socket_(io_), connected_(false), capturing_ser_(false)
{
}

// ================= CONNECT =================
bool TelnetClient::connect(const std::string& host, int port)
{
    try
    {
        tcp::endpoint endpoint(asio::ip::make_address(host), port);
        socket_.connect(endpoint);
        connected_ = true;

        std::cout << "=== Connected to " << host << ":" << port << " ===\n";
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Connection failed: " << e.what() << std::endl;
        return false;
    }
}

// ================= START RECEIVER =================
void TelnetClient::startReceiver()
{
    std::thread([this]() { receiveMessages(); }).detach();
}

// ================= GENERIC SEND =================
bool TelnetClient::SendCmdReceiveData(const std::string& cmd,
                                      std::string& outBuffer)
{
    if (!connected_)
        return false;

    try
    {
        outBuffer.clear();
        std::string fullCmd = cmd + "\n";

        if (cmd == "SER" || cmd == "SER GET_ALL")
        {
            capturing_ser_ = true;
            ser_buffer_.clear();
        }

        asio::write(socket_, asio::buffer(fullCmd));
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "SendCmdReceiveData error: "
                  << e.what() << std::endl;
        return false;
    }
}

// ================= FSM CALLBACK SETTER =================
void TelnetClient::setEventCallback(std::function<void(FsmEvent)> cb)
{
    eventCallback_ = std::move(cb);
}

// ================= IS CONNECTED =================
bool TelnetClient::isConnected() const
{
    return connected_;
}

// ================= RESPONSE ACCESS =================
const std::string& TelnetClient::getLastResponse() const
{
    return ser_buffer_;
}

// ================= RECEIVE THREAD =================
void TelnetClient::receiveMessages()
{
    try
    {
        while (connected_)
        {
            asio::streambuf buffer;
            boost::system::error_code ec;

            asio::read_until(socket_, buffer, '\n', ec);

            if (ec == asio::error::eof)
            {
                std::cout << "\n=== Server closed connection ===\n";
                break;
            }
            else if (ec)
            {
                std::cerr << "Receive error: " << ec.message() << std::endl;
                break;
            }

            std::istream is(&buffer);
            std::string line;

            while (std::getline(is, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();

                std::cout << line << std::endl;
                ser_buffer_ += line + "\n";

                // ================= LOGIN LEVEL 1 =================
                if (line.find("Login1 successful") != std::string::npos)
                {
                    if (eventCallback_)
                        eventCallback_(FsmEvent::Login1Ok);
                }
                else if (line.find("Login1 failed") != std::string::npos)
                {
                    if (eventCallback_)
                        eventCallback_(FsmEvent::Login1Fail);
                }

                // ================= LOGIN LEVEL 2 =================
                if (line.find("Login2 successful") != std::string::npos)
                {
                    if (eventCallback_)
                        eventCallback_(FsmEvent::Login2Ok);
                }
                else if (line.find("Login2 failed") != std::string::npos)
                {
                    if (eventCallback_)
                        eventCallback_(FsmEvent::Login2Fail);
                }

                // ================= SER COMPLETE =================
                if (capturing_ser_ &&
                    line.find("SER Response Complete") != std::string::npos)
                {
                    saveSERToFile(ser_buffer_);
                    capturing_ser_ = false;

                    if (eventCallback_)
                        eventCallback_(FsmEvent::SerOk);

                    ser_buffer_.clear();
                }

            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Receiver exception: " << e.what() << std::endl;
    }

    connected_ = false;
}

// ================= SAVE SER =================
void TelnetClient::saveSERToFile(const std::string& ser_data)
{
    try
    {
        std::ofstream file("ser_data.json");
        if (!file.is_open())
        {
            std::cerr << "Failed to open ser_data.json\n";
            return;
        }

        file << ser_data;
        file.close();
        std::cout << "\n[SER data saved to ser_data.json]\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << "SER save error: " << e.what() << std::endl;
    }
}
