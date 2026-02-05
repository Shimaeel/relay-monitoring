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

// ================= SEND COMMAND =================
bool TelnetClient::sendCommand(const std::string& command)
{
    if (!connected_)
        return false;

    try
    {
        std::string cmd = command + "\n";

        if (command == "SER" || command == "SER GET_ALL")
        {
            capturing_ser_ = true;
            ser_buffer_.clear();
        }

        asio::write(socket_, asio::buffer(cmd));
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Send error: " << e.what() << std::endl;
        return false;
    }
}

bool TelnetClient::isConnected() const
{
    return connected_;
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

                if (capturing_ser_)
                {
                    ser_buffer_ += line + "\n";

                    if (line.find("SER Response Complete") != std::string::npos)
                    {
                        saveSERToFile(ser_buffer_);
                        capturing_ser_ = false;
                        ser_buffer_.clear();
                    }
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
        std::vector<std::string> records;
        std::istringstream stream(ser_data);
        std::string line;

        while (std::getline(stream, line))
        {
            if (line.find("Record ") != std::string::npos)
                records.push_back(line);
        }

        std::ofstream file("ser_data.json");
        if (!file.is_open())
        {
            std::cerr << "Failed to open ser_data.json\n";
            return;
        }

        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&tt));

        file << "{\n";
        file << "  \"timestamp\": \"" << timestamp << "\",\n";
        file << "  \"totalRecords\": " << records.size() << ",\n";
        file << "  \"records\": [\n";

        for (size_t i = 0; i < records.size(); ++i)
        {
            file << "    { \"raw\": \"" << records[i] << "\" }";
            if (i + 1 < records.size())
                file << ",";
            file << "\n";
        }

        file << "  ]\n}\n";
        file.close();

        std::cout << "\n[SER data saved to ser_data.json]\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << "SER save error: " << e.what() << std::endl;
    }
}
