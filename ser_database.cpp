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

    // Create table if it doesn't exist
    if (!createTable())
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
            record_id TEXT NOT NULL,
            timestamp TEXT NOT NULL,
            status TEXT NOT NULL,
            description TEXT,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(record_id, timestamp)
        );
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

    return true;
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
    if (recordExists(record.record_id, record.timestamp))
    {
        return true; // Not an error, just skip duplicate
    }

    const char* sql = "INSERT INTO ser_records (record_id, timestamp, status, description) "
                      "VALUES (?, ?, ?, ?);";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        last_error_ = "Failed to prepare statement: " + std::string(sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_text(stmt, 1, record.record_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, record.timestamp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, record.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, record.description.c_str(), -1, SQLITE_TRANSIENT);

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
        if (recordExists(record.record_id, record.timestamp))
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

    const char* sql = "SELECT record_id, timestamp, status, description FROM ser_records "
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
        record.record_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        record.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        
        const char* desc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        record.description = desc ? desc : "";
        
        records.push_back(record);
    }

    sqlite3_finalize(stmt);
    return records;
}

/**
 * @brief Retrieve SER records filtered by status value.
 *
 * @details Executes a parameterised query with `WHERE status = ?`
 *          and returns results in descending timestamp order.
 *
 * @param status  Status string to match (e.g. "ASSERTED", "DEASSERTED").
 *
 * @return std::vector<SERRecord>  Matching records, newest first.
 *         Empty if none match, database is closed, or query fails.
 *
 * @pre isOpen() == true
 */
std::vector<SERRecord> SERDatabase::getRecordsByStatus(const std::string& status)
{
    std::vector<SERRecord> records;

    if (!db_)
    {
        last_error_ = "Database not open";
        return records;
    }

    const char* sql = "SELECT record_id, timestamp, status, description FROM ser_records "
                      "WHERE status = ? ORDER BY timestamp DESC;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        last_error_ = "Failed to prepare statement: " + std::string(sqlite3_errmsg(db_));
        return records;
    }

    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        SERRecord record;
        record.record_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        record.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        
        const char* desc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
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

// ================= HELPER FUNCTIONS =================

/**
 * @brief Check whether a record with the given ID and timestamp already exists.
 *
 * @details Uses a `SELECT 1 ... LIMIT 1` query for minimal overhead.
 *          Called internally by insertRecord() to prevent duplicates.
 *
 * @param recordId   The record identifier to look up.
 * @param timestamp  The timestamp to match against.
 *
 * @return true   A matching row exists.
 * @return false  No match found, or the query failed.
 */
bool SERDatabase::recordExists(const std::string& recordId, const std::string& timestamp)
{
    const char* sql = "SELECT 1 FROM ser_records WHERE record_id = ? AND timestamp = ? LIMIT 1;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        return false;
    }

    sqlite3_bind_text(stmt, 1, recordId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, timestamp.c_str(), -1, SQLITE_TRANSIENT);

    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);

    return exists;
}

// ================= EXPORT TO JSON =================

/**
 * @brief Export all SER records to a JSON file.
 *
 * @details Retrieves all records via getAllRecords() and writes them as
 *          a JSON array to the specified file. Each record object contains:
 *          `sno`, `date`, `time`, `element`, and `state` fields.
 *
 *          Special characters in the `description` field are escaped
 *          to produce valid JSON output.
 *
 * @param filePath  Destination file path (e.g. "data.json"). Created or
 *                  overwritten if it already exists.
 *
 * @return true   File written successfully.
 * @return false  Database not open or file cannot be opened for writing.
 *                Check getLastError() for details.
 *
 * @pre isOpen() == true
 *
 * @see SERJsonWriter Alternative streaming JSON writer.
 */
bool SERDatabase::exportToJSON(const std::string& filePath)
{
    if (!db_)
    {
        last_error_ = "Database not open";
        return false;
    }

    std::vector<SERRecord> records = getAllRecords();
    
    std::ofstream file(filePath);
    if (!file.is_open())
    {
        last_error_ = "Cannot open file for writing: " + filePath;
        return false;
    }

    file << "[\n";
    
    int sno = 1;
    for (size_t i = 0; i < records.size(); ++i)
    {
        const auto& rec = records[i];
        
        // Parse timestamp to extract date and time
        std::string date = rec.timestamp;
        std::string time = "";
        
        size_t spacePos = rec.timestamp.find(' ');
        if (spacePos != std::string::npos)
        {
            date = rec.timestamp.substr(0, spacePos);
            time = rec.timestamp.substr(spacePos + 1);
        }
        
        // Escape special characters in description
        std::string element = rec.description;
        for (size_t j = 0; j < element.length(); ++j)
        {
            if (element[j] == '"' || element[j] == '\\')
            {
                element.insert(j, "\\");
                ++j;
            }
        }
        
        file << "  {\n";
        file << "    \"sno\": " << sno++ << ",\n";
        file << "    \"date\": \"" << date << "\",\n";
        file << "    \"time\": \"" << time << "\",\n";
        file << "    \"element\": \"" << element << "\",\n";
        file << "    \"state\": \"" << rec.status << "\"\n";
        file << "  }";
        
        if (i < records.size() - 1)
            file << ",";
        
        file << "\n";
    }
    
    file << "]\n";
    file.close();
    
    return true;
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
