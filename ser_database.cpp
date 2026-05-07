// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file ser_database.cpp
 * @brief Implementation of SERDatabase class for SQLite storage
 * 
 * @details Provides SQLite-based persistent storage for System Event Records.
 * Implements all CRUD operations, duplicate detection, and JSON export.
 * 
 * ## Implementation Notes
 * 
 * ### Transaction Strategy
 * - Single inserts: Direct INSERT with duplicate check
 * - Bulk inserts: Wrapped in BEGIN/COMMIT for performance
 * - Duplicate detection before INSERT (not via ON CONFLICT)
 * 
 * ### Error Handling
 * - All SQLite errors captured in last_error_
 * - Operations return false/0 on error
 * - Database state remains consistent after errors
 * 
 * ### Query Performance
 * - Indexes on record_id, status, timestamp
 * - Results ordered by timestamp DESC
 * - UNIQUE constraint on (record_id, timestamp)
 * 
 * @see ser_database.hpp Header with class declaration
 * @see SQLite documentation: https://sqlite.org/docs.html
 */

#include "ser_database.hpp"
#include <iostream>

// ================= CONSTRUCTOR / DESTRUCTOR =================

/**
 * @brief Construct a new SERDatabase with the specified file path.
 *
 * @details Stores the database path for later use by open(). Does not
 * open or create the SQLite file until open() is called.
 *
 * @param dbPath  Filesystem path to the SQLite database file
 *                (e.g. "ser_records.db"). Created automatically by
 *                open() if it does not already exist.
 *
 * @post db_ == nullptr (not yet connected)
 */
SERDatabase::SERDatabase(const std::string& dbPath)
    : db_path_(dbPath), db_(nullptr)
{
}

/**
 * @brief Destroy the SERDatabase and release all resources.
 *
 * @details Calls close() to finalize the SQLite handle if still open.
 *          Safe to call even if the database was never opened.
 */
SERDatabase::~SERDatabase()
{
    close();
}

// ================= OPEN / CLOSE =================

/**
 * @brief Open the SQLite database and create the schema if needed.
 *
 * @details Performs the following steps:
 * 1. If already open, returns true immediately (idempotent).
 * 2. Calls sqlite3_open() to create/open the database file.
 * 3. Invokes createTable() to ensure the `ser_records` table and
 *    its indexes exist.
 *
 * @return true   Database opened (or was already open) and schema ready.
 * @return false  sqlite3_open() or createTable() failed.
 *                Check getLastError() for details.
 *
 * @post  On success: db_ != nullptr, isOpen() == true.
 * @post  On failure: db_ == nullptr, last_error_ set.
 *
 * @see close() Release the connection when done.
 * @see createTable() Internal schema initialisation.
 */
bool SERDatabase::open()
{
    if (db_)
    {
        return true; // Already open
    }

    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK)
    {
        last_error_ = "Cannot open database: " + std::string(sqlite3_errmsg(db_));
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    // Enable WAL mode for better concurrent read/write performance (24/7 operation)
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

    // Create table if it doesn't exist
    if (!createTable())
    {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    // Create settings_files / settings_entries tables for FILE SHOW / FILE READ
    if (!createSettingsTables())
    {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    return true;
}

/**
 * @brief Close the SQLite database connection.
 *
 * @details Finalizes the internal sqlite3 handle and sets db_ to nullptr.
 *          Safe to call multiple times; subsequent calls are no-ops.
 *
 * @post db_ == nullptr, isOpen() == false.
 */
void SERDatabase::close()
{
    if (db_)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

/**
 * @brief Check whether the database connection is currently open.
 *
 * @return true  Database is open and ready for queries.
 * @return false Database is closed.
 */
bool SERDatabase::isOpen() const
{
    return db_ != nullptr;
}

// ================= TABLE CREATION =================

/**
 * @brief Create the `ser_records` table and indexes if they do not exist.
 *
 * @details Executes a multi-statement SQL that creates:
 * - **Table** `ser_records` with columns:
 *   `id`, `record_id`, `timestamp`, `status`, `description`, `created_at`.
 *   A UNIQUE constraint on `(record_id, timestamp)` prevents duplicates.
 * - **Indexes** on `record_id`, `status`, and `timestamp` for fast lookups.
 *
 * @return true   Schema created or already existed.
 * @return false  SQL execution failed; last_error_ set.
 *
 * @pre db_ != nullptr (called internally by open()).
 */
bool SERDatabase::createTable()
{
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS ser_records (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            relay_id TEXT NOT NULL DEFAULT '',
            relay_name TEXT NOT NULL DEFAULT '',
            record_id TEXT NOT NULL,
            timestamp TEXT NOT NULL,
            status TEXT NOT NULL,
            description TEXT,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(relay_id, record_id, timestamp)
        );
        CREATE INDEX IF NOT EXISTS idx_relay_id ON ser_records(relay_id);
        CREATE INDEX IF NOT EXISTS idx_record_id ON ser_records(record_id);
        CREATE INDEX IF NOT EXISTS idx_status ON ser_records(status);
        CREATE INDEX IF NOT EXISTS idx_timestamp ON ser_records(timestamp);
    )";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);

    if (rc != SQLITE_OK)
    {
        last_error_ = "SQL error: " + std::string(errMsg);
        sqlite3_free(errMsg);
        return false;
    }

    // Migration: add relay columns to existing databases that don't have them
    sqlite3_exec(db_, "ALTER TABLE ser_records ADD COLUMN relay_id TEXT NOT NULL DEFAULT '';",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE ser_records ADD COLUMN relay_name TEXT NOT NULL DEFAULT '';",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "CREATE INDEX IF NOT EXISTS idx_relay_id ON ser_records(relay_id);",
                 nullptr, nullptr, nullptr);

    return true;
}

// ================= SETTINGS TABLES =================

/**
 * @brief Create settings_files and settings_entries tables + indexes.
 */
bool SERDatabase::createSettingsTables()
{
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS settings_files (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            relay_id      TEXT NOT NULL,
            relay_name    TEXT NOT NULL DEFAULT '',
            substation    TEXT NOT NULL DEFAULT '',
            bay           TEXT NOT NULL DEFAULT '',
            pse           TEXT NOT NULL DEFAULT '',
            breaker       TEXT NOT NULL DEFAULT '',
            file_name     TEXT NOT NULL,
            raw_content   TEXT NOT NULL DEFAULT '',
            content_hash  TEXT NOT NULL DEFAULT '',
            fetched_at    DATETIME DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(relay_id, file_name)
        );
        CREATE INDEX IF NOT EXISTS idx_sf_relay      ON settings_files(relay_id);
        CREATE INDEX IF NOT EXISTS idx_sf_substation ON settings_files(substation);
        CREATE INDEX IF NOT EXISTS idx_sf_filename   ON settings_files(file_name);

        CREATE TABLE IF NOT EXISTS settings_entries (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            file_id     INTEGER NOT NULL,
            relay_id    TEXT NOT NULL,
            relay_name  TEXT NOT NULL DEFAULT '',
            substation  TEXT NOT NULL DEFAULT '',
            bay         TEXT NOT NULL DEFAULT '',
            file_name   TEXT NOT NULL,
            section     TEXT NOT NULL DEFAULT '',
            key         TEXT NOT NULL,
            value       TEXT NOT NULL DEFAULT '',
            line_no     INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY(file_id) REFERENCES settings_files(id) ON DELETE CASCADE
        );
        CREATE INDEX IF NOT EXISTS idx_se_file    ON settings_entries(file_id);
        CREATE INDEX IF NOT EXISTS idx_se_relay   ON settings_entries(relay_id);
        CREATE INDEX IF NOT EXISTS idx_se_section ON settings_entries(section);
        CREATE INDEX IF NOT EXISTS idx_se_filename ON settings_entries(file_name);
    )";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        last_error_ = "settings tables SQL error: " + std::string(errMsg);
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

/**
 * @brief Insert (or replace) a settings file and its parsed entries.
 */
bool SERDatabase::insertSettingsFile(const SettingsFile& sf)
{
    if (!db_)
    {
        last_error_ = "Database not open";
        return false;
    }
    if (sf.relay_id.empty() || sf.file_name.empty())
    {
        last_error_ = "insertSettingsFile: relay_id and file_name required";
        return false;
    }

    sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    // 1. Delete previous rows for (relay_id, file_name)
    {
        const char* delEntries =
            "DELETE FROM settings_entries WHERE relay_id = ? AND file_name = ?;";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, delEntries, -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(st, 1, sf.relay_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, sf.file_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }

        const char* delFile =
            "DELETE FROM settings_files WHERE relay_id = ? AND file_name = ?;";
        if (sqlite3_prepare_v2(db_, delFile, -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(st, 1, sf.relay_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, sf.file_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }

    // 2. Insert into settings_files
    sqlite3_int64 file_id = 0;
    {
        const char* sql =
            "INSERT INTO settings_files "
            "(relay_id, relay_name, substation, bay, pse, breaker, "
            " file_name, raw_content, content_hash) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK)
        {
            last_error_ = "prepare settings_files insert: " + std::string(sqlite3_errmsg(db_));
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        sqlite3_bind_text(st, 1, sf.relay_id.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, sf.relay_name.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, sf.substation.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, sf.bay.c_str(),          -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, sf.pse.c_str(),          -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 6, sf.breaker.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 7, sf.file_name.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 8, sf.raw_content.c_str(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 9, sf.content_hash.c_str(), -1, SQLITE_TRANSIENT);

        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE)
        {
            last_error_ = "settings_files insert failed: " + std::string(sqlite3_errmsg(db_));
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        file_id = sqlite3_last_insert_rowid(db_);
    }

    // 3. Insert entries
    {
        const char* sql =
            "INSERT INTO settings_entries "
            "(file_id, relay_id, relay_name, substation, bay, file_name, "
            " section, key, value, line_no) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK)
        {
            last_error_ = "prepare settings_entries insert: " + std::string(sqlite3_errmsg(db_));
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }

        for (const auto& e : sf.entries)
        {
            sqlite3_reset(st);
            sqlite3_clear_bindings(st);

            sqlite3_bind_int64(st, 1, file_id);
            sqlite3_bind_text(st, 2, sf.relay_id.c_str(),    -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, sf.relay_name.c_str(),  -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 4, sf.substation.c_str(),  -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 5, sf.bay.c_str(),         -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 6, sf.file_name.c_str(),   -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 7, e.section.c_str(),      -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 8, e.key.c_str(),          -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 9, e.value.c_str(),        -1, SQLITE_TRANSIENT);
            sqlite3_bind_int (st, 10, e.line_no);

            int rc = sqlite3_step(st);
            if (rc != SQLITE_DONE)
            {
                last_error_ = "settings_entries insert failed: " + std::string(sqlite3_errmsg(db_));
                sqlite3_finalize(st);
                sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
                return false;
            }
        }
        sqlite3_finalize(st);
    }

    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    return true;
}

/**
 * @brief Retrieve all settings files (metadata only) for a relay.
 */
std::vector<SettingsFile> SERDatabase::getSettingsFiles(const std::string& relay_id)
{
    std::vector<SettingsFile> out;
    if (!db_)
    {
        last_error_ = "Database not open";
        return out;
    }

    std::string sql =
        "SELECT relay_id, relay_name, substation, bay, pse, breaker, "
        "       file_name, raw_content, content_hash "
        "FROM settings_files";
    if (!relay_id.empty())
        sql += " WHERE relay_id = ?";
    sql += " ORDER BY relay_id, file_name;";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
    {
        last_error_ = "getSettingsFiles prepare: " + std::string(sqlite3_errmsg(db_));
        return out;
    }
    if (!relay_id.empty())
        sqlite3_bind_text(st, 1, relay_id.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(st) == SQLITE_ROW)
    {
        SettingsFile sf;
        auto col = [&](int i) -> std::string {
            const unsigned char* t = sqlite3_column_text(st, i);
            return t ? reinterpret_cast<const char*>(t) : "";
        };
        sf.relay_id     = col(0);
        sf.relay_name   = col(1);
        sf.substation   = col(2);
        sf.bay          = col(3);
        sf.pse          = col(4);
        sf.breaker      = col(5);
        sf.file_name    = col(6);
        sf.raw_content  = col(7);
        sf.content_hash = col(8);
        out.push_back(std::move(sf));
    }
    sqlite3_finalize(st);
    return out;
}

/**
 * @brief Retrieve parsed entries for one settings file.
 */
std::vector<SettingsEntry> SERDatabase::getSettingsEntries(const std::string& relay_id,
                                                           const std::string& file_name)
{
    std::vector<SettingsEntry> out;
    if (!db_)
    {
        last_error_ = "Database not open";
        return out;
    }

    const char* sql =
        "SELECT section, key, value, line_no "
        "FROM settings_entries "
        "WHERE relay_id = ? AND file_name = ? "
        "ORDER BY line_no;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK)
    {
        last_error_ = "getSettingsEntries prepare: " + std::string(sqlite3_errmsg(db_));
        return out;
    }
    sqlite3_bind_text(st, 1, relay_id.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, file_name.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(st) == SQLITE_ROW)
    {
        SettingsEntry e;
        const unsigned char* s = sqlite3_column_text(st, 0); e.section = s ? reinterpret_cast<const char*>(s) : "";
        const unsigned char* k = sqlite3_column_text(st, 1); e.key     = k ? reinterpret_cast<const char*>(k) : "";
        const unsigned char* v = sqlite3_column_text(st, 2); e.value   = v ? reinterpret_cast<const char*>(v) : "";
        e.line_no = sqlite3_column_int(st, 3);
        out.push_back(std::move(e));
    }
    sqlite3_finalize(st);
    return out;
}

// ================= INSERT OPERATIONS =================

/**
 * @brief Insert a single SER record into the database.
 *
 * @details First calls recordExists() to check for duplicates on
 *          `(record_id, timestamp)`. If the record already exists it
 *          returns true (skip, not an error). Otherwise it prepares an
 *          INSERT statement with parameter binding for safety.
 *
 * @param record  The SERRecord to insert. All four fields
 *                (record_id, timestamp, status, description) are stored.
 *
 * @return true   Record inserted or already existed (duplicate skip).
 * @return false  Database not open, prepare failed, or step failed.
 *                Check getLastError() for details.
 *
 * @pre isOpen() == true
 *
 * @see insertRecords() Bulk insert with transaction wrapping.
 * @see recordExists()  Duplicate detection helper.
 */
bool SERDatabase::insertRecord(const SERRecord& record)
{
    if (!db_)
    {
        last_error_ = "Database not open";
        return false;
    }

    // Skip if record already exists
    if (recordExists(record.relay_id, record.record_id, record.timestamp))
    {
        return true; // Not an error, just skip duplicate
    }

    const char* sql = "INSERT INTO ser_records (relay_id, relay_name, record_id, timestamp, status, description) "
                      "VALUES (?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        last_error_ = "Failed to prepare statement: " + std::string(sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_text(stmt, 1, record.relay_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, record.relay_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, record.record_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, record.timestamp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, record.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, record.description.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        last_error_ = "Failed to insert record: " + std::string(sqlite3_errmsg(db_));
        return false;
    }

    return true;
}

/**
 * @brief Bulk-insert a vector of SER records inside a single transaction.
 *
 * @details Wraps all inserts in `BEGIN TRANSACTION / COMMIT` for
 *          significantly better performance on large batches. Each
 *          record is inserted via insertRecord(), so duplicates are
 *          silently skipped.
 *
 * @param records  Vector of SERRecord objects to insert.
 *
 * @return int  Number of records successfully inserted (excludes duplicates).
 *              Returns 0 if the database is not open.
 *
 * @pre isOpen() == true
 *
 * @note The entire batch is committed even if some individual inserts
 *       are skipped as duplicates.
 */
int SERDatabase::insertRecords(const std::vector<SERRecord>& records)
{
    if (!db_)
    {
        last_error_ = "Database not open";
        return 0;
    }

    int inserted = 0;

    // Begin transaction for better performance
    sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    for (const auto& record : records)
    {
        if (insertRecord(record))
        {
            inserted++;
        }
    }

    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);

    return inserted;
}

/**
 * @brief Insert records and return only the newly inserted ones.
 *
 * @details Wraps all inserts in a single transaction. For each record,
 *          checks whether it already exists before inserting. Only records
 *          that did not previously exist and were successfully inserted are
 *          included in the returned vector.
 *
 * @param records        Vector of SERRecord objects to insert.
 * @param insertedCount  [out] Number of records actually inserted.
 *
 * @return std::vector<SERRecord>  Records that were newly inserted (not duplicates).
 *
 * @pre isOpen() == true
 */
std::vector<SERRecord> SERDatabase::insertAndGetNewRecords(
    const std::vector<SERRecord>& records, int& insertedCount)
{
    insertedCount = 0;
    std::vector<SERRecord> newRecords;

    if (!db_)
    {
        last_error_ = "Database not open";
        return newRecords;
    }

    sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    for (const auto& record : records)
    {
        // Check if already exists BEFORE inserting
        if (recordExists(record.relay_id, record.record_id, record.timestamp))
            continue;   // duplicate — skip

        if (insertRecord(record))
        {
            ++insertedCount;
            newRecords.push_back(record);
        }
    }

    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);

    return newRecords;
}

// ================= QUERY OPERATIONS =================

/**
 * @brief Retrieve all SER records ordered by timestamp (most recent first).
 *
 * @details Executes `SELECT ... ORDER BY timestamp DESC` and maps each
 *          row into a SERRecord struct. Returns an empty vector if the
 *          database is not open or the query fails.
 *
 * @return std::vector<SERRecord>  All stored records, newest first.
 *         Empty if database is closed or an error occurs.
 *
 * @pre isOpen() == true
 *
 * @see getRecordsByStatus() Filtered version of this query.
 */
std::vector<SERRecord> SERDatabase::getAllRecords()
{
    std::vector<SERRecord> records;

    if (!db_)
    {
        last_error_ = "Database not open";
        return records;
    }

    const char* sql = "SELECT relay_id, relay_name, record_id, timestamp, status, description FROM ser_records "
                      "ORDER BY timestamp DESC;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        last_error_ = "Failed to prepare statement: " + std::string(sqlite3_errmsg(db_));
        return records;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        SERRecord record;
        record.relay_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        record.relay_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.record_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        record.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        record.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        
        const char* desc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        record.description = desc ? desc : "";
        
        records.push_back(record);
    }

    sqlite3_finalize(stmt);
    return records;
}

/**
 * @brief Get the total number of SER records in the database.
 *
 * @details Executes `SELECT COUNT(*) FROM ser_records`.
 *
 * @return int  Number of rows, or 0 if the database is closed or
 *              the query fails.
 *
 * @pre isOpen() == true
 */
int SERDatabase::getRecordCount()
{
    if (!db_)
    {
        last_error_ = "Database not open";
        return 0;
    }

    const char* sql = "SELECT COUNT(*) FROM ser_records;";
    sqlite3_stmt* stmt;
    int count = 0;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    return count;
}

// ================= DELETE OPERATIONS =================

/**
 * @brief Delete all SER records from the database.
 *
 * @details Executes `DELETE FROM ser_records`. The table structure
 *          and indexes remain intact; only data rows are removed.
 *
 * @return true   All records deleted successfully.
 * @return false  Database not open or SQL error. Check getLastError().
 *
 * @pre isOpen() == true
 *
 * @warning This operation cannot be undone. All historical SER data
 *          will be permanently lost.
 */
bool SERDatabase::clearAllRecords()
{
    if (!db_)
    {
        last_error_ = "Database not open";
        return false;
    }

    const char* sql = "DELETE FROM ser_records;";
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);

    if (rc != SQLITE_OK)
    {
        last_error_ = "SQL error: " + std::string(errMsg);
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}

// ================= PRUNING =================

/**
 * @brief Delete records older than the specified number of days.
 *
 * @details Deletes rows whose `created_at` timestamp is older than
 *          `now - days`.  Uses SQLite's `datetime()` function for
 *          portable date arithmetic.  Intended for periodic housekeeping
 *          during 24/7 continuous operation.
 *
 * @param days  Number of days to retain (default: 90).
 *
 * @return int  Number of records deleted, or -1 on error.
 *
 * @pre isOpen() == true
 */
int SERDatabase::pruneOldRecords(int days)
{
    if (!db_)
    {
        last_error_ = "Database not open";
        return -1;
    }

    const char* sql = "DELETE FROM ser_records WHERE created_at < datetime('now', ?);";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        last_error_ = "Failed to prepare prune statement: " + std::string(sqlite3_errmsg(db_));
        return -1;
    }

    std::string modifier = "-" + std::to_string(days) + " days";
    sqlite3_bind_text(stmt, 1, modifier.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        last_error_ = "Failed to prune records: " + std::string(sqlite3_errmsg(db_));
        return -1;
    }

    int deleted = sqlite3_changes(db_);
    if (deleted > 0)
        std::cout << "[DB] Pruned " << deleted << " records older than " << days << " days\n";
    return deleted;
}

// ================= HELPER FUNCTIONS =================

/**
 * @brief Check whether a record with the given relay ID, record ID and timestamp already exists.
 *
 * @details Uses a `SELECT 1 ... LIMIT 1` query for minimal overhead.
 *          Called internally by insertRecord() to prevent duplicates.
 *
 * @param relayId    The relay identifier to look up.
 * @param recordId   The record identifier to look up.
 * @param timestamp  The timestamp to match against.
 *
 * @return true   A matching row exists.
 * @return false  No match found, or the query failed.
 */
bool SERDatabase::recordExists(const std::string& relayId, const std::string& recordId, const std::string& timestamp)
{
    const char* sql = "SELECT 1 FROM ser_records WHERE relay_id = ? AND record_id = ? AND timestamp = ? LIMIT 1;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        return false;
    }

    sqlite3_bind_text(stmt, 1, relayId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, recordId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, timestamp.c_str(), -1, SQLITE_TRANSIENT);

    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);

    return exists;
}

/**
 * @brief Get the error message from the last failed operation.
 *
 * @details Set by any operation that fails (open, insert, query, export).
 *          Contains the SQLite error message or a custom description.
 *          Empty if no error has occurred.
 *
 * @return const std::string&  Reference to the last error message.
 */
const std::string& SERDatabase::getLastError() const
{
    return last_error_;
}
