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

SERDatabase::SERDatabase(const std::string& dbPath)
    : db_path_(dbPath), db_(nullptr)
{
}

SERDatabase::~SERDatabase()
{
    close();
}

// ================= OPEN / CLOSE =================

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

void SERDatabase::close()
{
    if (db_)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool SERDatabase::isOpen() const
{
    return db_ != nullptr;
}

// ================= TABLE CREATION =================

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

// ================= QUERY OPERATIONS =================

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

const std::string& SERDatabase::getLastError() const
{
    return last_error_;
}
