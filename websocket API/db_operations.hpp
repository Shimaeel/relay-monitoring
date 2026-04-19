// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file db_operations.hpp
 * @brief Generic SQLite database operations — standalone free functions.
 *
 * @details Provides reusable, mutex-protected SQLite operations that accept
 * a raw `sqlite3*` handle and a `std::mutex&`.  Each function returns a JSON
 * response string suitable for sending over WebSocket or any other transport.
 *
 * These functions are transport-agnostic: they know nothing about WebSocket,
 * HTTP, or any networking layer.
 *
 * ## Operations
 *
 * | Function          | Purpose                                   |
 * |-------------------|-------------------------------------------|
 * | `dbGetAll()`      | Fetch entire table                        |
 * | `dbGetIncremental()` | Rows where `rowid > sinceRowId`        |
 * | `dbQuery()`       | Arbitrary SELECT with bind parameters     |
 * | `dbExec()`        | INSERT / UPDATE / DELETE / DDL            |
 * | `dbTables()`      | List user tables                          |
 * | `dbSchema()`      | Column metadata (PRAGMA table_info)       |
 * | `dbDefine()`      | Create table from JSON column definitions |
 *
 * ## Usage
 *
 * ```cpp
 * #include "db_operations.hpp"
 *
 * sqlite3* db = ...;
 * std::mutex mtx;
 *
 * std::string json = wsdb_ops::dbGetAll(42, db, mtx, "ser_records");
 * // json = {"id":42,"ok":true,"columns":[...],"rows":[...],...}
 * ```
 *
 * @see wsdb_json.hpp    JSON helpers used by these operations
 * @see ws_db_server.hpp WebSocket server that dispatches to these functions
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include "dll_export.hpp"
#include "wsdb_json.hpp"

#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <sqlite3.h>

namespace wsdb_ops {

// ── helpers (internal) ───────────────────────────────────────────────────

namespace detail {

/**
 * @brief Validate a SQL identifier (table or column name).
 *
 * @details Only alphanumeric characters and underscores are allowed.
 * This prevents SQL injection in dynamically constructed queries.
 *
 * @param name The identifier to validate.
 * @return true  @p name is non-empty and contains only [A-Za-z0-9_].
 * @return false @p name is empty or contains disallowed characters.
 */
inline bool validName(const std::string& name)
{
    for (char ch : name)
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') return false;
    return !name.empty();
}

/**
 * @brief Build a JSON error response.
 *
 * @param id   Request id echoed back to the caller.
 * @param msg  Human-readable error description.
 * @return JSON string: @c {"id":<id>,"ok":false,"error":"<msg>"}.
 */
inline std::string errorResp(int64_t id, const std::string& msg)
{
    std::ostringstream out;
    out << "{\"id\":" << id << ",\"ok\":false,\"error\":\"" << wsdb_json::escape(msg) << "\"}";
    return out.str();
}

/**
 * @brief Execute a SELECT query and return results as a JSON string.
 *
 * @details Prepares and steps through the statement, collecting column
 * names and row data.  Integers and floats are emitted unquoted; text
 * and blob values are JSON-escaped and quoted.  The response includes
 * a @c maxRowId field tracking the highest @c rowid seen (useful for
 * incremental polling).
 *
 * @param id     Request id echoed in the response.
 * @param db     Open SQLite database handle.
 * @param mtx    Mutex protecting @p db (locked for the duration).
 * @param sql    SQL SELECT statement (may contain @c ? placeholders).
 * @param params Bind-parameter values, bound as text.
 * @return JSON string:
 *         @c {"id":<id>,"ok":true,"columns":[...],"rows":[...],"rowCount":N,"maxRowId":M}
 *         or an error response on failure.
 */
inline std::string runSelect(int64_t id, sqlite3* db, std::mutex& mtx,
                             const std::string& sql,
                             const std::vector<std::string>& params)
{
    std::lock_guard<std::mutex> lock(mtx);
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return errorResp(id, sqlite3_errmsg(db));

    for (int i = 0; i < static_cast<int>(params.size()); ++i)
        sqlite3_bind_text(stmt, i + 1, params[i].c_str(), -1, SQLITE_TRANSIENT);

    int colCount = sqlite3_column_count(stmt);
    std::vector<std::string> columns;
    columns.reserve(colCount);
    for (int c = 0; c < colCount; ++c)
        columns.push_back(sqlite3_column_name(stmt, c));

    int64_t maxRowId = 0;
    std::ostringstream rows;
    rows << '[';
    bool firstRow = true;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        if (!firstRow) rows << ',';
        firstRow = false;
        rows << '[';
        for (int c = 0; c < colCount; ++c)
        {
            if (c) rows << ',';
            int ctype = sqlite3_column_type(stmt, c);
            if (ctype == SQLITE_NULL)
            {
                rows << "null";
            }
            else if (ctype == SQLITE_INTEGER || ctype == SQLITE_FLOAT)
            {
                const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, c));
                rows << (t ? t : "null");
            }
            else
            {
                const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, c));
                rows << '"' << wsdb_json::escape(t ? t : "") << '"';
            }
        }
        rows << ']';

        // Track max rowid (first column if it's "rowid")
        if (colCount > 0)
        {
            const char* col0 = sqlite3_column_name(stmt, 0);
            if (col0 && std::string(col0) == "rowid")
            {
                int64_t rid = sqlite3_column_int64(stmt, 0);
                if (rid > maxRowId) maxRowId = rid;
            }
        }
    }
    rows << ']';
    sqlite3_finalize(stmt);

    std::ostringstream out;
    out << "{\"id\":" << id << ",\"ok\":true,\"columns\":[";
    for (size_t c = 0; c < columns.size(); ++c)
    {
        if (c) out << ',';
        out << '"' << wsdb_json::escape(columns[c]) << '"';
    }
    out << "],\"rows\":" << rows.str()
        << ",\"rowCount\":" << (firstRow ? 0 : -1)  // placeholder
        << ",\"maxRowId\":" << maxRowId << '}';

    // Fix rowCount
    std::string result = out.str();
    {
        size_t cnt = 0;
        if (!firstRow)
        {
            std::string rowStr = rows.str();
            int depth = 0;
            for (char ch : rowStr)
            {
                if (ch == '[') ++depth;
                else if (ch == ']') --depth;
                else if (ch == ',' && depth == 1) ++cnt;
            }
            cnt += 1;
        }
        auto rcPos = result.find("\"rowCount\":");
        if (rcPos != std::string::npos)
        {
            auto startPos = rcPos + 11;
            auto endPos = result.find(',', startPos);
            if (endPos == std::string::npos) endPos = result.find('}', startPos);
            result.replace(startPos, endPos - startPos, std::to_string(cnt));
        }
    }
    return result;
}

} // namespace detail

// ═════════════════════════════════════════════════════════════════════════
//  Public API — one function per action
// ═════════════════════════════════════════════════════════════════════════

/**
 * @brief Fetch every row in a table.
 * @param id     Request id (echoed in response)
 * @param db     SQLite handle
 * @param mtx    Mutex protecting the handle
 * @param table  Table name
 * @return JSON response string
 */
inline std::string dbGetAll(int64_t id, sqlite3* db, std::mutex& mtx,
                            const std::string& table)
{
    if (table.empty()) return detail::errorResp(id, "Missing \"table\"");
    if (!detail::validName(table)) return detail::errorResp(id, "Invalid table name");

    std::string sql = "SELECT rowid, * FROM " + table + " ORDER BY rowid;";
    return detail::runSelect(id, db, mtx, sql, {});
}

/**
 * @brief Fetch rows added since a given rowid (incremental update).
 * @param id          Request id
 * @param db          SQLite handle
 * @param mtx         DB mutex
 * @param table       Table name
 * @param sinceRowId  Only return rows with rowid > this value
 * @return JSON response string
 */
inline std::string dbGetIncremental(int64_t id, sqlite3* db, std::mutex& mtx,
                                    const std::string& table, int64_t sinceRowId)
{
    if (table.empty()) return detail::errorResp(id, "Missing \"table\"");
    if (!detail::validName(table)) return detail::errorResp(id, "Invalid table name");

    std::string sql = "SELECT rowid, * FROM " + table +
                      " WHERE rowid > ? ORDER BY rowid;";
    return detail::runSelect(id, db, mtx, sql, {std::to_string(sinceRowId)});
}

/**
 * @brief Run an arbitrary SELECT query with bind parameters.
 * @param id      Request id
 * @param db      SQLite handle
 * @param mtx     DB mutex
 * @param sql     SQL SELECT statement
 * @param params  Bind parameter values
 * @return JSON response string
 */
inline std::string dbQuery(int64_t id, sqlite3* db, std::mutex& mtx,
                           const std::string& sql,
                           const std::vector<std::string>& params)
{
    if (sql.empty()) return detail::errorResp(id, "Missing \"sql\"");
    return detail::runSelect(id, db, mtx, sql, params);
}

/**
 * @brief Execute a write statement (INSERT / UPDATE / DELETE / DDL).
 * @param id      Request id
 * @param db      SQLite handle
 * @param mtx     DB mutex
 * @param sql     SQL statement
 * @param params  Bind parameter values
 * @return JSON response string with changes count and lastRowId
 */
inline std::string dbExec(int64_t id, sqlite3* db, std::mutex& mtx,
                          const std::string& sql,
                          const std::vector<std::string>& params)
{
    if (sql.empty()) return detail::errorResp(id, "Missing \"sql\"");

    std::lock_guard<std::mutex> lock(mtx);
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return detail::errorResp(id, sqlite3_errmsg(db));
    for (int i = 0; i < static_cast<int>(params.size()); ++i)
        sqlite3_bind_text(stmt, i + 1, params[i].c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) return detail::errorResp(id, sqlite3_errmsg(db));

    int changes = sqlite3_changes(db);
    int64_t lastId = sqlite3_last_insert_rowid(db);

    std::ostringstream out;
    out << "{\"id\":" << id
        << ",\"ok\":true,\"changes\":" << changes
        << ",\"lastRowId\":" << lastId << '}';
    return out.str();
}

/**
 * @brief List all user tables in the database.
 * @param id   Request id
 * @param db   SQLite handle
 * @param mtx  DB mutex
 * @return JSON response string with tables array
 */
inline std::string dbTables(int64_t id, sqlite3* db, std::mutex& mtx)
{
    std::lock_guard<std::mutex> lock(mtx);
    const char* sql = "SELECT name FROM sqlite_master WHERE type='table' "
                      "AND name NOT LIKE 'sqlite_%' ORDER BY name;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return detail::errorResp(id, sqlite3_errmsg(db));

    std::ostringstream out;
    out << "{\"id\":" << id << ",\"ok\":true,\"tables\":[";
    bool first = true;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        if (!first) out << ',';
        first = false;
        const char* n = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        out << '"' << wsdb_json::escape(n ? n : "") << '"';
    }
    sqlite3_finalize(stmt);
    out << "]}";
    return out.str();
}

/**
 * @brief Get column metadata for a table (PRAGMA table_info).
 * @param id     Request id
 * @param db     SQLite handle
 * @param mtx    DB mutex
 * @param table  Table name
 * @return JSON response string with columns array
 */
inline std::string dbSchema(int64_t id, sqlite3* db, std::mutex& mtx,
                            const std::string& table)
{
    if (table.empty())  return detail::errorResp(id, "Missing \"table\"");
    if (!detail::validName(table)) return detail::errorResp(id, "Invalid table name");

    std::lock_guard<std::mutex> lock(mtx);
    std::string sql = "PRAGMA table_info(" + table + ");";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return detail::errorResp(id, sqlite3_errmsg(db));

    std::ostringstream out;
    out << "{\"id\":" << id << ",\"ok\":true,\"columns\":[";
    bool first = true;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        if (!first) out << ',';
        first = false;
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        int notnull = sqlite3_column_int(stmt, 3);
        const char* dflt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        int pk = sqlite3_column_int(stmt, 5);
        out << "{\"name\":\"" << wsdb_json::escape(name ? name : "") << '"'
            << ",\"type\":\"" << wsdb_json::escape(type ? type : "") << '"'
            << ",\"notnull\":" << notnull
            << ",\"dflt_value\":" << (dflt ? "\"" + wsdb_json::escape(dflt) + "\"" : "null")
            << ",\"pk\":" << pk << '}';
    }
    sqlite3_finalize(stmt);
    out << "]}";
    return out.str();
}

/**
 * @brief Create a table from JSON column definitions.
 *
 * @details Each column object may have:
 * - `name` (required), `type` (default "TEXT")
 * - `pk`, `autoincrement`, `notnull`, `unique` (booleans)
 * - `default` (raw SQL default expression)
 *
 * @param id          Request id
 * @param db          SQLite handle
 * @param mtx         DB mutex
 * @param table       Table name
 * @param columnsJson Raw JSON string of the request (must contain "columns" array)
 * @return JSON response string with generated CREATE TABLE SQL
 */
inline std::string dbDefine(int64_t id, sqlite3* db, std::mutex& mtx,
                            const std::string& table,
                            const std::string& columnsJson)
{
    if (table.empty())     return detail::errorResp(id, "Missing \"table\"");
    if (!detail::validName(table)) return detail::errorResp(id, "Invalid table name");

    auto colDefs = wsdb_json::getObjectArray(columnsJson, "columns");
    if (colDefs.empty()) return detail::errorResp(id, "Missing or empty \"columns\" array");

    std::ostringstream ddl;
    ddl << "CREATE TABLE IF NOT EXISTS " << table << " (\n";
    for (size_t i = 0; i < colDefs.size(); ++i)
    {
        if (i) ddl << ",\n";
        const auto& c = colDefs[i];
        std::string name = wsdb_json::getString(c, "name");
        std::string type = wsdb_json::getString(c, "type");
        if (name.empty()) return detail::errorResp(id, "Column missing \"name\"");
        if (type.empty()) type = "TEXT";

        ddl << "    " << name << " " << type;
        if (wsdb_json::getBool(c, "pk"))      ddl << " PRIMARY KEY";
        if (wsdb_json::getBool(c, "autoincrement") && wsdb_json::getBool(c, "pk"))
                                               ddl << " AUTOINCREMENT";
        if (wsdb_json::getBool(c, "notnull"))  ddl << " NOT NULL";
        if (wsdb_json::getBool(c, "unique"))   ddl << " UNIQUE";
        std::string def = wsdb_json::getString(c, "default");
        if (!def.empty()) ddl << " DEFAULT " << def;
    }
    ddl << "\n);";

    std::string createSql = ddl.str();

    std::lock_guard<std::mutex> lock(mtx);
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, createSql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        std::string err = errMsg ? errMsg : "Unknown error";
        sqlite3_free(errMsg);
        return detail::errorResp(id, err);
    }

    std::ostringstream out;
    out << "{\"id\":" << id << ",\"ok\":true,\"sql\":\"" << wsdb_json::escape(createSql) << "\"}";
    return out.str();
}

} // namespace wsdb_ops
