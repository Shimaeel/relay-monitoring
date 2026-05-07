// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file settings_record.hpp
 * @brief Settings file structures and INI-style parser for SEL relay settings.
 *
 * @details Defines structures for representing the contents of a single
 * settings file fetched from a relay (via FILE SHOW <name> on SEL-735 or
 * FILE READ on SEL-451 over Ymodem) and a parser that converts the raw
 * text into structured key/value entries grouped by section.
 *
 * ## Source Format
 *
 * SEL relay settings files are INI-style with two value-line flavours:
 *
 * @code
 * [INFO]                              ← section header
 * RELAYTYPE=SEL-451-5                 ← KEY=VALUE flavour (header section)
 * FID=SEL-451-5-R310-V0-Z015012-D2012
 * PARTNO=04515615XC2X43344XXXX
 *
 * [D5]                                ← another section
 * MINDIST,"OFF"                       ← KEY,"VALUE" flavour (CSV-quoted)
 * BI_1,"RLYDIS"
 * BI_2,"TRIPLED"
 * @endcode
 *
 * The parser handles both flavours and emits a SettingsEntry per
 * key/value line tagged with its enclosing section.
 *
 * @see SERDatabase::insertSettingsFile() Persistence layer
 * @see PipelineProcessingWorker FILE SHOW response handler
 */

#pragma once

#include "dll_export.hpp"
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>

#include "ser_record.hpp"  // for sanitizeSerLine()

/**
 * @struct SettingsEntry
 * @brief A single parsed key/value pair from a settings file.
 */
struct TELNET_SML_API SettingsEntry
{
    std::string section;   ///< Section header (e.g. "INFO", "D5", "GROUP1")
    std::string key;       ///< Setting name (e.g. "RELAYTYPE", "BI_1")
    std::string value;     ///< Setting value (e.g. "SEL-451-5", "RLYDIS")
    int line_no{0};        ///< 1-based line number in the source file
};

/**
 * @struct SettingsFile
 * @brief One settings file with its parsed entries and metadata.
 *
 * @details `relay_id`, `relay_name`, and substation/device identity are
 * stamped by the pipeline layer (the parser does not know about relays).
 */
struct TELNET_SML_API SettingsFile
{
    std::string relay_id;       ///< Relay identifier (e.g. "1")
    std::string relay_name;     ///< Relay display name (e.g. "SEL-451")
    std::string substation;     ///< Substation name (e.g. "Substation Alpha")
    std::string bay;            ///< Bay identifier (e.g. "Bay 1")
    std::string pse;            ///< Primary system equipment label
    std::string breaker;        ///< Breaker identifier

    std::string file_name;      ///< File name as listed by FILE DIR (e.g. "SET_G1.TXT")
    std::string raw_content;    ///< Complete original text of the file
    std::string content_hash;   ///< Hash of raw_content for change detection (optional)

    std::vector<SettingsEntry> entries;  ///< Parsed key/value rows

    SettingsFile() = default;
};

/**
 * @brief Trim leading/trailing whitespace (including \r) from a string in place.
 */
inline void _settingsTrim(std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) { s.clear(); return; }
    size_t end = s.find_last_not_of(" \t\r\n");
    s = s.substr(start, end - start + 1);
}

/**
 * @brief Strip surrounding double quotes from a value, if present.
 *
 * @details Used for the CSV-quoted flavour — `BI_1,"RLYDIS"` → value
 * is the raw token `"RLYDIS"` which we want to surface as `RLYDIS`.
 */
inline std::string _settingsUnquote(const std::string& s)
{
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

/**
 * @brief Parse a settings file's raw text into a vector of SettingsEntry.
 *
 * @details Recognises three line types:
 *   - `[SECTION]`             → updates current section
 *   - `KEY=VALUE`             → INFO-style entry (no quotes)
 *   - `KEY,"VALUE"` / `KEY,V` → CSV-style entry (value optionally quoted)
 *
 * Lines that match none of the above (blank lines, comments, malformed
 * rows) are silently skipped so the parser stays tolerant of relay
 * pagination artefacts and trailing prompts.
 *
 * @param raw  Complete file text as returned by `FILE SHOW <name>`.
 * @return std::vector<SettingsEntry>  Entries in source order.
 *
 * @note The current section defaults to empty string until the first
 *       `[HEADER]` line is encountered.
 */
inline std::vector<SettingsEntry> parseSettingsFile(const std::string& raw)
{
    std::vector<SettingsEntry> entries;
    std::istringstream stream(raw);
    std::string line;
    std::string current_section;
    int line_no = 0;

    while (std::getline(stream, line))
    {
        ++line_no;
        line = sanitizeSerLine(line);
        _settingsTrim(line);

        if (line.empty())
            continue;

        // Skip relay prompts that may be embedded in the response
        if (line == "=>" || line == "=>>" || line == ">" || line == "=")
            continue;

        // Section header: [NAME]
        if (line.front() == '[' && line.back() == ']')
        {
            current_section = line.substr(1, line.size() - 2);
            _settingsTrim(current_section);
            continue;
        }

        // KEY=VALUE flavour (INFO section style)
        size_t eq = line.find('=');
        size_t comma = line.find(',');

        if (eq != std::string::npos && (comma == std::string::npos || eq < comma))
        {
            std::string key   = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            _settingsTrim(key);
            _settingsTrim(value);
            if (key.empty()) continue;

            entries.push_back(SettingsEntry{current_section, key, value, line_no});
            continue;
        }

        // KEY,"VALUE" or KEY,VALUE flavour (data rows)
        if (comma != std::string::npos)
        {
            std::string key   = line.substr(0, comma);
            std::string value = line.substr(comma + 1);
            _settingsTrim(key);
            _settingsTrim(value);
            value = _settingsUnquote(value);
            if (key.empty()) continue;

            entries.push_back(SettingsEntry{current_section, key, value, line_no});
            continue;
        }

        // Unrecognised line — skip silently
    }

    return entries;
}
