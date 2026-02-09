#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

/**
 * @brief Structure representing a System Event Record (SER)
 */
struct SERRecord
{
    std::string record_id;    // e.g., "SER-001"
    std::string timestamp;    // e.g., "2026-01-14 09:15:23"
    std::string status;       // e.g., "ACTIVE" or "INACTIVE"
    std::string description;  // e.g., "Voltage threshold exceeded"

    SERRecord() = default;

    SERRecord(const std::string& id, const std::string& ts,
              const std::string& stat, const std::string& desc)
        : record_id(id), timestamp(ts), status(stat), description(desc)
    {
    }
};

/**
 * @brief Parse SER response string into vector of SERRecord
 * 
 * Expected format:
 * Total Records: 5
 * Record 1: SER-001 | 2026-01-14 09:15:23 | ACTIVE | Voltage threshold exceeded
 * Record 2: SER-002 | 2026-01-14 09:20:45 | ACTIVE | Temperature anomaly detected
 * ...
 * SER Response Complete
 */
inline std::vector<SERRecord> parseSERResponse(const std::string& response)
{
    std::vector<SERRecord> records;
    std::istringstream stream(response);
    std::string line;

    while (std::getline(stream, line))
    {
        // Remove \r if present
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        // Skip empty lines and non-record lines
        if (line.empty() || line.find("Record ") != 0)
            continue;

        // Find the colon after "Record N:"
        size_t colonPos = line.find(':');
        if (colonPos == std::string::npos)
            continue;

        // Get the part after "Record N: "
        std::string recordData = line.substr(colonPos + 2);

        // Split by " | "
        std::vector<std::string> parts;
        size_t start = 0;
        size_t delimPos;
        
        while ((delimPos = recordData.find(" | ", start)) != std::string::npos)
        {
            parts.push_back(recordData.substr(start, delimPos - start));
            start = delimPos + 3; // Skip " | "
        }
        parts.push_back(recordData.substr(start)); // Last part

        if (parts.size() >= 4)
        {
            SERRecord record;
            record.record_id = parts[0];
            record.timestamp = parts[1];
            record.status = parts[2];
            record.description = parts[3];
            records.push_back(record);
        }
    }

    return records;
}
