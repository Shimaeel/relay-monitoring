/**
 * @file ser_database.hpp
 * @brief SQLite Database Handler for SER Records
 * 
 * @details This header defines the SERDatabase class for persistent storage
 * of System Event Records using SQLite. Provides CRUD operations, querying,
 * and JSON export for UI integration.
 * 
 * ## Database Schema
 * 
 * @dot
 * digraph Schema {
 *     node [shape=record, style=filled, fillcolor=lightyellow];
 *     
 *     ser_records [label="{ser_records|
 *         id : INTEGER PRIMARY KEY\l
 *         record_id : TEXT NOT NULL\l
 *         timestamp : TEXT NOT NULL\l
 *         status : TEXT NOT NULL\l
 *         description : TEXT\l
 *         created_at : DATETIME\l
 *         |UNIQUE(record_id, timestamp)\l
 *     }"];
 *     
 *     idx1 [label="idx_record_id", shape=ellipse, fillcolor=lightblue];
 *     idx2 [label="idx_status", shape=ellipse, fillcolor=lightblue];
 *     idx3 [label="idx_timestamp", shape=ellipse, fillcolor=lightblue];
 *     
 *     ser_records -> idx1;
 *     ser_records -> idx2;
 *     ser_records -> idx3;
 * }
 * @enddot
 * 
 * ## Data Flow Architecture
 * 
 * @dot
 * digraph DataFlow {
 *     rankdir=LR;
 *     node [shape=box, style=filled, fillcolor=lightyellow];
 *     
 *     FSM [label="TelnetFSM\nPollSerAction"];
 *     Parser [label="parseSERResponse()"];
 *     DB [label="SERDatabase"];
 *     SQLite [label="SQLite File\n(.db)", shape=cylinder, fillcolor=lightblue];
 *     JSON [label="data.json", shape=note];
 *     WS [label="WebSocket\nServer"];
 *     UI [label="Web UI"];
 *     
 *     FSM -> Parser;
 *     Parser -> DB [label="insertRecords()"];
 *     DB -> SQLite [label="SQL"];
 *     DB -> JSON [label="exportToJSON()"];
 *     DB -> WS [label="getAllRecords()"];
 *     WS -> UI;
 * }
 * @enddot
 * 
 * ## Usage Example
 * 
 * @code{.cpp}
 * SERDatabase db("ser_records.db");
 * if (db.open()) {
 *     // Insert records
 *     std::vector<SERRecord> records = parseSERResponse(response);
 *     int count = db.insertRecords(records);
 *     
 *     // Query records
 *     auto allRecords = db.getAllRecords();
 *     auto activeRecords = db.getRecordsByStatus("Asserted");
 *     
 *     // Export for UI
 *     db.exportToJSON("ui/data.json");
 * }
 * @endcode
 * 
 * @see SERRecord Structure for individual records
 * @see PollSerAction FSM action that stores records
 * @see SERWebSocketServer Real-time data access
 * 
 * @note Thread-safe for single writer, multiple readers
 * @note Duplicate records (same record_id + timestamp) are silently skipped
 * 
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include "dll_export.hpp"
#include <string>
#include <vector>
#include <fstream>
#include <sqlite3.h>

#include "ser_record.hpp"

/**
 * @class SERDatabase
 * @brief SQLite database handler for storing and retrieving SER records
 * 
 * @details Manages persistent storage of System Event Records using SQLite.
 * Features:
 * - Automatic database and table creation
 * - Duplicate detection (skips existing record_id + timestamp combinations)
 * - Transaction support for bulk inserts
 * - Multiple query options (all, by status)
 * - JSON export for web UI
 * 
 * ## Lifecycle
 * 
 * @msc
 * App,DB,SQLite;
 * App->DB [label="create"];
 * App->DB [label="open()"];
 * DB->SQLite [label="sqlite3_open"];
 * DB->SQLite [label="CREATE TABLE"];
 * App->DB [label="insertRecords()"];
 * DB->SQLite [label="BEGIN; INSERT; COMMIT"];
 * App->DB [label="getAllRecords()"];
 * DB->SQLite [label="SELECT"];
 * SQLite->DB [label="rows"];
 * DB->App [label="vector<SERRecord>"];
 * App->DB [label="close() / destructor"];
 * DB->SQLite [label="sqlite3_close"];
 * @endmsc
 * 
 * @invariant db_ is either nullptr (closed) or valid SQLite handle
 * @invariant Table exists after successful open()
 */
class TELNET_SML_API SERDatabase
{
public:
    /**
     * @brief Construct database handler with specified file path
     * 
     * @details Creates handler but does not open database. Call open() to
     * establish connection. File will be created if it doesn't exist.
     * 
     * @param dbPath Path to SQLite database file (default: "ser_records.db")
     * 
     * @post db_ == nullptr (not yet connected)
     * @post db_path_ == dbPath
     */
    explicit SERDatabase(const std::string& dbPath = "ser_records.db");
    
    /**
     * @brief Destructor - automatically closes database connection
     * 
     * @details Calls close() to release SQLite resources.
     */
    ~SERDatabase();

    /// @name Non-copyable
    /// @{
    SERDatabase(const SERDatabase&) = delete;
    SERDatabase& operator=(const SERDatabase&) = delete;
    /// @}

    /**
     * @brief Open database connection and initialize schema
     * 
     * @details Opens SQLite database file and creates table if not exists.
     * Also creates indexes for efficient querying.
     * 
     * @return true Database opened and table created successfully
     * @return false Failed to open or create table (check getLastError())
     * 
     * @post On success: isOpen() == true, table exists
     * @post On failure: isOpen() == false, last_error_ contains message
     * 
     * @see createTable() Internal table creation
     */
    bool open();

    /**
     * @brief Close database connection
     * 
     * @details Releases SQLite handle. Safe to call multiple times.
     * 
     * @post isOpen() == false
     * @post db_ == nullptr
     */
    void close();

    /**
     * @brief Check if database is currently open
     * 
     * @return true Database connection is active
     * @return false Database is closed
     */
    bool isOpen() const;

    /**
     * @brief Insert a single SER record into database
     * 
     * @details Inserts record if not already present (by record_id + timestamp).
     * Duplicate records are silently skipped (returns true).
     * 
     * @param record The SER record to insert
     * 
     * @return true Record inserted or already exists
     * @return false Insert failed (check getLastError())
     * 
     * @pre isOpen() == true
     */
    bool insertRecord(const SERRecord& record);

    /**
     * @brief Insert multiple SER records with transaction support
     * 
     * @details Inserts all records within a transaction for better performance.
     * Skips duplicates and continues with remaining records.
     * 
     * @param records Vector of SER records to insert
     * 
     * @return int Number of records successfully inserted (0 to records.size())
     * 
     * @pre isOpen() == true
     * 
     * @note Uses BEGIN/COMMIT transaction for performance
     * @note Duplicates are counted as "success" (not errors)
     */
    int insertRecords(const std::vector<SERRecord>& records);

    /**
     * @brief Retrieve all SER records ordered by timestamp (newest first)
     * 
     * @return std::vector<SERRecord> All stored records, or empty if error/none
     * 
     * @pre isOpen() == true
     */
    std::vector<SERRecord> getAllRecords();

    /**
     * @brief Retrieve records filtered by status
     * 
     * @param status Status to filter by (e.g., "Asserted", "Deasserted")
     * 
     * @return std::vector<SERRecord> Matching records ordered by timestamp
     * 
     * @pre isOpen() == true
     */
    std::vector<SERRecord> getRecordsByStatus(const std::string& status);

    /**
     * @brief Get total count of records in database
     * 
     * @return int Number of records, or 0 if error
     * 
     * @pre isOpen() == true
     */
    int getRecordCount();

    /**
     * @brief Delete all records from database
     * 
     * @return true Records cleared successfully
     * @return false Clear failed (check getLastError())
     * 
     * @pre isOpen() == true
     * @post getRecordCount() == 0
     */
    bool clearAllRecords();

    /**
     * @brief Export all records to JSON file for UI consumption
     * 
     * @details Generates JSON array compatible with web UI format.
     * 
     * Output Format:
     * @code{.json}
     * [
     *   {
     *     "sno": 45,
     *     "date": "02/14/22",
     *     "time": "12:47:19.970",
     *     "element": "Power loss",
     *     "state": ""
     *   },
     *   ...
     * ]
     * @endcode
     * 
     * @param filePath Path to output JSON file (default: "ui/data.json")
     * 
     * @return true Export successful
     * @return false Export failed (check getLastError())
     * 
     * @pre isOpen() == true
     */
    bool exportToJSON(const std::string& filePath = "ui/data.json");

    /**
     * @brief Get last error message
     * 
     * @return const std::string& Error message from last failed operation
     */
    const std::string& getLastError() const;

private:
    /**
     * @brief Create SER records table and indexes
     * 
     * @details Creates table with schema:
     * - id: Auto-increment primary key
     * - record_id: Event number from relay
     * - timestamp: Event date and time
     * - status: Event state (Asserted/Deasserted/empty)
     * - description: Event element/description
     * - created_at: When record was stored
     * 
     * Also creates indexes on record_id, status, and timestamp.
     * 
     * @return true Table created or already exists
     * @return false Creation failed
     */
    bool createTable();

    /**
     * @brief Check if record already exists in database
     * 
     * @param recordId Record ID to check
     * @param timestamp Timestamp to check
     * 
     * @return true Record with same ID and timestamp exists
     * @return false Record not found
     */
    bool recordExists(const std::string& recordId, const std::string& timestamp);

private:
    std::string db_path_;      ///< Path to SQLite database file
    sqlite3* db_;               ///< SQLite database handle (nullptr when closed)
    std::string last_error_;   ///< Last error message for getLastError()
};
