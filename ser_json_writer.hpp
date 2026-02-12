/**
 * @file ser_json_writer.hpp
 * @brief JSON Serialization and File Output for SER Records
 *
 * @details This header provides utility functions for converting SER records
 * to JSON format and writing to files for web UI consumption.
 *
 * ## Output Format
 *
 * Generates JSON array compatible with Tabulator.js:
 * ```json
 * [
 *   {
 *     "sno": 45,
 *     "date": "02/14/22",
 *     "time": "12:47:19.970",
 *     "element": "SALARM",
 *     "state": "Asserted"
 *   },
 *   ...
 * ]
 * ```
 *
 * ## Data Flow
 *
 * @dot
 * digraph JSONWriter {
 *     rankdir=LR;
 *     node [shape=box, style=filled, fillcolor=lightyellow];
 *
 *     Records [label="vector<SERRecord>"];
 *     ToJSON [label="recordsToJsonTable()"];
 *     WriteFile [label="writeJsonToFile()"];
 *     Output [label="ui/data.json", shape=note, fillcolor=lightblue];
 *     UI [label="Web Browser\nTabulator.js", shape=ellipse];
 *
 *     Records -> ToJSON [label="convert"];
 *     ToJSON -> WriteFile [label="JSON string"];
 *     WriteFile -> Output [label="write"];
 *     Output -> UI [label="HTTP fetch"];
 * }
 * @enddot
 *
 * @see SharedSerReader Consumer that uses these functions
 * @see SERRecord Input data structure
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include <fstream>
#include <string>
#include <vector>

#include "ser_record.hpp"

/**
 * @brief Escapes special characters for JSON string values.
 *
 * @details Escapes double quotes and backslashes to ensure valid JSON output.
 *
 * @param value Input string to escape
 * @return std::string Escaped string safe for JSON embedding
 */
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

/**
 * @brief Converts SER records to JSON array string.
 *
 * @details Formats records as JSON array with fields:
 * - sno: Record ID (numeric if parseable, string otherwise)
 * - date: Date portion of timestamp (before space)
 * - time: Time portion of timestamp (after space)
 * - element: Description field
 * - state: Status field
 *
 * @param records Vector of SERRecord to convert
 * @return std::string JSON array string
 *
 * @note Timestamp is automatically split into date and time
 * @note All string values are JSON-escaped
 */
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

/**
 * @brief Writes JSON string to file.
 *
 * @details Opens file in binary mode with truncation and writes JSON content.
 *
 * @param path Output file path
 * @param json JSON string to write
 * @param[out] error Optional pointer to receive error message
 *
 * @return true Successfully wrote file
 * @return false Failed (error message in @p error if provided)
 *
 * @note Opens file with std::ios::binary to preserve line endings
 * @note Truncates existing file content
 */
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
