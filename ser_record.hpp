#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>

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
 * Actual relay format:
 * #      Date      Time           Element           State
 *     45 02/14/22  12:47:19.970   Power loss
 *     43 12/30/25  15:49:30.860   SALARM            Asserted
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

        // Skip empty lines and header lines
        if (line.empty() || line.find('#') == 0 || line.find("Date") != std::string::npos)
            continue;

        // Trim leading whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos)
            continue;

        // Try to parse: "45 02/14/22  12:47:19.970   Power loss"
        // Format: number date time element [state]
        
        // Check if line starts with a number (SER record)
        if (!std::isdigit(line[start]))
            continue;

        // Parse using position-based extraction
        std::istringstream lineStream(line);
        
        int recordNum;
        std::string date, time, element, state;
        
        // Read record number, date, time
        if (!(lineStream >> recordNum >> date >> time))
            continue;

        // Read rest of line as element + optional state
        std::string rest;
        std::getline(lineStream, rest);
        
        // Trim leading whitespace from rest
        size_t restStart = rest.find_first_not_of(" \t");
        if (restStart != std::string::npos)
            rest = rest.substr(restStart);
        else
            rest = "";

        // Check if last word is a state (Asserted/Deasserted)
        size_t lastSpace = rest.rfind(' ');
        if (lastSpace != std::string::npos)
        {
            std::string lastWord = rest.substr(lastSpace + 1);
            // Trim trailing whitespace from lastWord
            size_t endPos = lastWord.find_last_not_of(" \t\r\n");
            if (endPos != std::string::npos)
                lastWord = lastWord.substr(0, endPos + 1);
                
            if (lastWord == "Asserted" || lastWord == "Deasserted")
            {
                state = lastWord;
                element = rest.substr(0, lastSpace);
                // Trim trailing whitespace from element
                size_t elemEnd = element.find_last_not_of(" \t");
                if (elemEnd != std::string::npos)
                    element = element.substr(0, elemEnd + 1);
            }
            else
            {
                element = rest;
                state = "";
            }
        }
        else
        {
            element = rest;
            state = "";
        }

        if (element.empty())
            continue;

        // Keep original date format from relay (MM/DD/YY)
        std::string formattedDate = date;

        SERRecord record;
        record.record_id = std::to_string(recordNum);  // Keep original # from relay
        record.timestamp = formattedDate + " " + time;
        record.status = state;  // Keep original state (Asserted/Deasserted/empty)
        record.description = element;
        records.push_back(record);
    }

    return records;
}
