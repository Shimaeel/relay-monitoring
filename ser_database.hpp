#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <sqlite3.h>

#include "ser_record.hpp"

/**
 * @brief SQLite database handler for storing SER records
 */
class SERDatabase
{
public:
    /**
     * @brief Construct database handler
     * @param dbPath Path to SQLite database file (created if not exists)
     */
    explicit SERDatabase(const std::string& dbPath = "ser_records.db");
    
    ~SERDatabase();

    // Prevent copying
    SERDatabase(const SERDatabase&) = delete;
    SERDatabase& operator=(const SERDatabase&) = delete;

    /**
     * @brief Open database connection and create table if needed
     * @return true on success
     */
    bool open();

    /**
     * @brief Close database connection
     */
    void close();

    /**
     * @brief Check if database is open
     */
    bool isOpen() const;

    /**
     * @brief Insert a single SER record
     * @param record The SER record to insert
     * @return true on success
     */
    bool insertRecord(const SERRecord& record);

    /**
     * @brief Insert multiple SER records
     * @param records Vector of SER records to insert
     * @return Number of records successfully inserted
     */
    int insertRecords(const std::vector<SERRecord>& records);

    /**
     * @brief Retrieve all SER records from database
     * @return Vector of all stored records
     */
    std::vector<SERRecord> getAllRecords();

    /**
     * @brief Retrieve records by status (ACTIVE/INACTIVE)
     * @param status Status to filter by
     * @return Vector of matching records
     */
    std::vector<SERRecord> getRecordsByStatus(const std::string& status);

    /**
     * @brief Get count of records in database
     * @return Number of records
     */
    int getRecordCount();

    /**
     * @brief Clear all records from database
     * @return true on success
     */
    bool clearAllRecords();

    /**
     * @brief Export records to JSON file for UI
     * @param filePath Path to output JSON file
     * @return true on success
     */
    bool exportToJSON(const std::string& filePath = "ui/data.json");

    /**
     * @brief Get last error message
     */
    const std::string& getLastError() const;

private:
    /**
     * @brief Create the SER records table if it doesn't exist
     */
    bool createTable();

    /**
     * @brief Check if record already exists (by record_id and timestamp)
     */
    bool recordExists(const std::string& recordId, const std::string& timestamp);

private:
    std::string db_path_;
    sqlite3* db_;
    std::string last_error_;
};
