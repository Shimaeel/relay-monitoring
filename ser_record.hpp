// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file ser_record.hpp
 * @brief System Event Record (SER) data structure and parser.
 *
 * @details Defines the SERRecord structure and a parser that converts raw
 * Telnet SER responses into structured records.
 *
 * ## SER Record Format
 *
 * Substation relays output SER data in the following format:
 * ```
 * #      Date      Time           Element           State
 *     45 02/14/22  12:47:19.970   Power loss
 *     43 12/30/25  15:49:30.860   SALARM            Asserted
 *     42 12/30/25  15:49:30.850   VMIN              Deasserted
 * ```
 *
 * ## Data Flow
 *
 * @dot
 * digraph SERFlow {
 *     rankdir=LR;
 *     node [shape=box, style=filled, fillcolor=lightyellow];
 *
 *     Relay [label="Relay\n(Raw Text)"];
 *     Parser [label="parseSERResponse()"];
 *     Records [label="vector<SERRecord>"];
 *     DB [label="SERDatabase"];
 *
 *     Relay -> Parser [label="text response"];
 *     Parser -> Records [label="parse"];
 *     Records -> DB [label="store"];
 * }
 * @enddot
 *
 * ## Usage Example
 *
 * @code{.cpp}
 * std::string response = "45 02/14/22 12:47:19.970 Power loss\n";
 * auto records = parseSERResponse(response);
 *
 * for (const auto& rec : records) {
 *     std::cout << rec.record_id << ": " << rec.description << "\n";
 * }
 * @endcode
 *
 * @see SERDatabase Database storage for SER records
 * @see PollSerAction FSM action that uses the parser
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include "dll_export.hpp"
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>

/**
 * @struct SERRecord
 * @brief Structure representing a System Event Record (SER).
 * 
 * @details Holds parsed data from a single SER entry retrieved from
 * a substation relay device. Each record represents one event in the
 * relay's event log.
 * 
 * ## Field Mapping
 * 
 * | Field | Source | Example |
 * |-------|--------|---------|
 * | relay_id | Pipeline config | "1" |
 * | relay_name | Pipeline config | "SEL-751" |
 * | record_id | Event # | "45" |
 * | timestamp | Date + Time | "02/14/22 12:47:19.970" |
 * | status | State | "Asserted" or "" |
 * | description | Element | "SALARM" |
 * 
 * @note record_id is stored as string to preserve original relay numbering
 * @note status may be empty for events without assertion state
 * @note relay_id and relay_name identify which relay produced this record
 */
struct TELNET_SML_API SERRecord
{
    std::string relay_id;     ///< Relay identifier (e.g., "1", "2") — set by pipeline
    std::string relay_name;   ///< Relay display name (e.g., "SEL-751") — set by pipeline
    std::string record_id;    ///< Record identifier (e.g., "45", "SER-001")
    std::string timestamp;    ///< Event timestamp (e.g., "02/14/22 12:47:19.970")
    std::string status;       ///< Event state ("Asserted", "Deasserted", or empty)
    std::string description;  ///< Event element/description (e.g., "SALARM", "Power loss")

    /**
     * @brief Default constructor.
     * @details Creates an empty SERRecord with all fields initialized to empty strings.
     */
    SERRecord() = default;

    /**
     * @brief Legacy parameterized constructor (no relay info).
     *
     * @param id Record identifier
     * @param ts Timestamp string
     * @param stat Status/state string
     * @param desc Description/element string
     */
    SERRecord(const std::string& id, const std::string& ts,
              const std::string& stat, const std::string& desc)
        : record_id(id), timestamp(ts), status(stat), description(desc)
    {
    }

    /**
     * @brief Full parameterized constructor with relay identification.
     *
     * @param rid  Relay identifier (e.g., "1")
     * @param rname Relay display name (e.g., "SEL-751")
     * @param id   Record identifier
     * @param ts   Timestamp string
     * @param stat Status/state string
     * @param desc Description/element string
     */
    SERRecord(const std::string& rid, const std::string& rname,
              const std::string& id, const std::string& ts,
              const std::string& stat, const std::string& desc)
        : relay_id(rid), relay_name(rname)
        , record_id(id), timestamp(ts), status(stat), description(desc)
    {
    }
};

/**
 * @brief Parse SER response string into a vector of SERRecord.
 *
 * @details Parses raw text response from the SER command into structured records.
 * Handles the specific format output by substation relay devices.
 * 
 * ## Input Format
 * 
 * ```
 * #      Date      Time           Element           State
 *     45 02/14/22  12:47:19.970   Power loss
 *     43 12/30/25  15:49:30.860   SALARM            Asserted
 * ```
 * 
 * ## Parsing Algorithm
 * 
 * @dot
 * digraph ParseAlgo {
 *     rankdir=TB;
 *     node [shape=box];
 *     
 *     start [shape=ellipse, label="Start"];
 *     readline [label="Read Line"];
 *     skip [label="Skip Header/Empty?", shape=diamond];
 *     parse [label="Parse: # Date Time Element [State]"];
 *     hasstate [label="Has State?", shape=diamond];
 *     addrecord [label="Add to Vector"];
 *     more [label="More Lines?", shape=diamond];
 *     done [shape=ellipse, label="Return Records"];
 *     
 *     start -> readline;
 *     readline -> skip;
 *     skip -> readline [label="Yes"];
 *     skip -> parse [label="No"];
 *     parse -> hasstate;
 *     hasstate -> addrecord [label="Split element/state"];
 *     hasstate -> addrecord [label="No state"];
 *     addrecord -> more;
 *     more -> readline [label="Yes"];
 *     more -> done [label="No"];
 * }
 * @enddot
 * 
 * ## Parsing Rules
 * 
 * 1. Lines starting with '#' are skipped (header)
 * 2. Lines containing "Date" are skipped (column headers)
 * 3. Lines must start with a number (record #)
 * 4. State detection: "Asserted" or "Deasserted" at end
 * 5. Whitespace is trimmed from all fields
 * 
 * @param response Raw text response from the SER command
 * @return std::vector<SERRecord> Parsed records (may be empty if parsing fails)
 *
 * @note Date format is preserved from relay (MM/DD/YY)
 * @note Empty lines and non-record lines are silently skipped
 * 
 * @see SERRecord Structure for individual records
 * @see SERDatabase::insertRecords() For storing parsed records
 */
inline std::string sanitizeSerLine(const std::string& line)
{
    std::string out;
    out.reserve(line.size());

    for (std::size_t i = 0; i < line.size(); ++i)
    {
        unsigned char c = static_cast<unsigned char>(line[i]);

        // Strip Telnet IAC sequences (0xFF cmd opt)
        if (c == 0xFF)
        {
            if (i + 1 < line.size() && static_cast<unsigned char>(line[i + 1]) == 0xFF)
            {
                ++i;
                continue;
            }
            if (i + 2 < line.size())
            {
                i += 2;
                continue;
            }
            break;
        }

        // Strip ANSI escape sequences (ESC [ ...)
        if (c == 0x1B)
        {
            if (i + 1 < line.size() && line[i + 1] == '[')
            {
                i += 2;
                while (i < line.size())
                {
                    unsigned char esc = static_cast<unsigned char>(line[i]);
                    if (esc >= 0x40 && esc <= 0x7E)
                        break;
                    ++i;
                }
                continue;
            }
            continue;
        }

        // Keep printable ASCII and tabs; drop other control chars
        if (c >= 0x20 || c == '\t')
            out.push_back(static_cast<char>(c));
    }

    return out;
}

inline std::vector<SERRecord> parseSERResponse(const std::string& response)
{
    std::vector<SERRecord> records;
    std::istringstream stream(response);
    std::string line;

    while (std::getline(stream, line))
    {
        line = sanitizeSerLine(line);

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
        size_t digitPos = line.find_first_of("0123456789", start);
        if (digitPos == std::string::npos)
            continue;
        if (digitPos != start)
            line = line.substr(digitPos);

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
