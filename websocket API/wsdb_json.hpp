// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file wsdb_json.hpp
 * @brief Lightweight JSON parsing and serialisation helpers (no external dependency).
 *
 * @details Provides minimal JSON utilities used by the WebSocket database server
 * and SQLite operation handlers.  Designed for flat JSON objects with string,
 * number, boolean values and simple arrays. Not a general-purpose JSON parser.
 *
 * ## Capabilities
 *
 * | Function         | Purpose                                              |
 * |------------------|------------------------------------------------------|
 * | `escape()`       | Escape a string for safe JSON embedding              |
 * | `getString()`    | Extract a string value by key                        |
 * | `getInt()`       | Extract an integer value by key                      |
 * | `getBool()`      | Extract a boolean value by key                       |
 * | `getStringArray()` | Extract a flat array of strings by key             |
 * | `getObjectArray()` | Extract an array of JSON objects (as raw strings)  |
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wsdb_json {

/**
 * @brief Escape a string for safe embedding inside a JSON string value.
 *
 * @details Replaces special characters (quotes, backslash, control chars)
 *          with their JSON escape sequences. The output is safe to embed
 *          between double-quote delimiters in a JSON document.
 *
 * @param s  The raw input string to escape.
 *
 * @return std::string  Escaped string ready for JSON serialisation.
 */
inline std::string escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s)
    {
        switch (ch)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += ch;     break;
        }
    }
    return out;
}

/**
 * @brief Extract a string value for a top-level key in a flat JSON object.
 *
 * @details Performs a simple text search for `"key":` and extracts the
 *          value that follows. Handles both quoted strings and bare
 *          values (numbers, booleans, null). Supports basic escape
 *          sequences inside quoted values.
 *
 * @param json  The JSON string to search.
 * @param key   The key name to look up (without quotes).
 *
 * @return std::string  The extracted value (unquoted), or an empty
 *                      string if the key is not found.
 *
 * @note Only searches top-level keys; does not recurse into nested objects.
 */
inline std::string getString(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size()) return {};

    if (json[pos] == '"')
    {
        ++pos;
        std::string val;
        while (pos < json.size() && json[pos] != '"')
        {
            if (json[pos] == '\\' && pos + 1 < json.size())
            {
                ++pos;
                switch (json[pos]) {
                case '"':  val += '"';  break;
                case '\\': val += '\\'; break;
                case 'n':  val += '\n'; break;
                case 'r':  val += '\r'; break;
                case 't':  val += '\t'; break;
                default:   val += json[pos]; break;
                }
            }
            else val += json[pos];
            ++pos;
        }
        return val;
    }
    // Non-string value (number, bool, null)
    std::string val;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != ']')
    {
        if (json[pos] != ' ' && json[pos] != '\t' && json[pos] != '\n' && json[pos] != '\r')
            val += json[pos];
        ++pos;
    }
    return val;
}

/**
 * @brief Extract an integer value for a top-level key.
 *
 * @details Calls getString() internally and converts the result to a
 *          64-bit signed integer via std::stoll(). Returns the default
 *          value if the key is absent or conversion fails.
 *
 * @param json        The JSON string to search.
 * @param key         The key name to look up.
 * @param defaultVal  Value returned when the key is missing or invalid
 *                    (default: 0).
 *
 * @return int64_t  The parsed integer value, or defaultVal on failure.
 */
inline int64_t getInt(const std::string& json, const std::string& key, int64_t defaultVal = 0)
{
    std::string val = getString(json, key);
    if (val.empty()) return defaultVal;
    try { return std::stoll(val); }
    catch (...) { return defaultVal; }
}

/**
 * @brief Extract a flat JSON array of strings for a top-level key.
 *
 * @details Locates `"key": [...]` and parses each element as a string.
 *          Handles both quoted string elements and bare values.
 *          Does not support nested arrays or objects within the array.
 *
 * @param json  The JSON string to search.
 * @param key   The key name whose array value to extract.
 *
 * @return std::vector<std::string>  Array elements as strings.
 *         Empty vector if the key is not found or has no array value.
 */
inline std::vector<std::string> getStringArray(const std::string& json, const std::string& key)
{
    std::vector<std::string> result;
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return result;
    pos = json.find('[', pos + needle.size());
    if (pos == std::string::npos) return result;
    ++pos;

    while (pos < json.size())
    {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ',' || json[pos] == '\t'
               || json[pos] == '\n' || json[pos] == '\r'))
            ++pos;
        if (pos >= json.size() || json[pos] == ']') break;

        if (json[pos] == '"')
        {
            ++pos;
            std::string val;
            while (pos < json.size() && json[pos] != '"')
            {
                if (json[pos] == '\\' && pos + 1 < json.size()) { ++pos; val += json[pos]; }
                else val += json[pos];
                ++pos;
            }
            if (pos < json.size()) ++pos;
            result.push_back(val);
        }
        else
        {
            std::string val;
            while (pos < json.size() && json[pos] != ',' && json[pos] != ']')
            {
                if (json[pos] != ' ' && json[pos] != '\t') val += json[pos];
                ++pos;
            }
            if (!val.empty()) result.push_back(val);
        }
    }
    return result;
}

/**
 * @brief Extract an array of JSON objects, each returned as a raw string.
 *
 * @details Locates `"key": [...]` and extracts each `{...}` block as an
 *          unparsed substring. Handles nested braces correctly via a
 *          depth counter. Each raw object string can be further parsed
 *          with getString() / getInt() etc.
 *
 * @param json  The JSON string to search.
 * @param key   The key name whose object-array value to extract.
 *
 * @return std::vector<std::string>  Each element is a raw JSON object
 *         string (including outer braces). Empty if key is absent.
 */
inline std::vector<std::string> getObjectArray(const std::string& json, const std::string& key)
{
    std::vector<std::string> result;
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return result;
    pos = json.find('[', pos + needle.size());
    if (pos == std::string::npos) return result;
    ++pos;

    while (pos < json.size())
    {
        while (pos < json.size() && json[pos] != '{' && json[pos] != ']') ++pos;
        if (pos >= json.size() || json[pos] == ']') break;

        // Find matching '}'
        size_t start = pos;
        int depth = 0;
        while (pos < json.size())
        {
            if (json[pos] == '{') ++depth;
            else if (json[pos] == '}') { --depth; if (depth == 0) { ++pos; break; } }
            ++pos;
        }
        result.push_back(json.substr(start, pos - start));
    }
    return result;
}

/**
 * @brief Check if a key's boolean value is true (or a truthy string/number).
 *
 * @details Calls getString() internally and treats `"true"` or `"1"` as
 *          truthy. All other values (including absent keys) return the
 *          default value.
 *
 * @param json        The JSON string to search.
 * @param key         The key name to look up.
 * @param defaultVal  Value returned when the key is missing (default: false).
 *
 * @return true   Value is `"true"` or `"1"`.
 * @return false  Value is anything else or key is absent.
 */
inline bool getBool(const std::string& json, const std::string& key, bool defaultVal = false)
{
    std::string val = getString(json, key);
    if (val.empty()) return defaultVal;
    return val == "true" || val == "1";
}

} // namespace wsdb_json
