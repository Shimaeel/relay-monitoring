#pragma once

#include <fstream>
#include <string>
#include <vector>

#include "ser_record.hpp"

inline std::string escapeJson(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8U);
    for (char ch : value)
    {
        if (ch == '"' || ch == '\\')
        {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

inline std::string recordsToJsonTable(const std::vector<SERRecord>& records)
{
    std::string json = "[\n";

    for (std::size_t i = 0; i < records.size(); ++i)
    {
        const auto& rec = records[i];
        std::string date = rec.timestamp;
        std::string time;

        std::size_t spacePos = rec.timestamp.find(' ');
        if (spacePos != std::string::npos)
        {
            date = rec.timestamp.substr(0, spacePos);
            time = rec.timestamp.substr(spacePos + 1U);
        }

        json += "  {\n";
        json += "    \"sno\": ";
        try
        {
            int value = std::stoi(rec.record_id);
            json += std::to_string(value);
        }
        catch (...)
        {
            json += "\"" + escapeJson(rec.record_id) + "\"";
        }
        json += ",\n";
        json += "    \"date\": \"" + escapeJson(date) + "\",\n";
        json += "    \"time\": \"" + escapeJson(time) + "\",\n";
        json += "    \"element\": \"" + escapeJson(rec.description) + "\",\n";
        json += "    \"state\": \"" + escapeJson(rec.status) + "\"\n";
        json += "  }";

        if (i + 1U < records.size())
            json += ",";

        json += "\n";
    }

    json += "]";
    return json;
}

inline bool writeJsonToFile(const std::string& path, const std::string& json, std::string* error = nullptr)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        if (error)
            *error = "Failed to open JSON output file";
        return false;
    }

    out.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!out)
    {
        if (error)
            *error = "Failed to write JSON output file";
        return false;
    }

    return true;
}
