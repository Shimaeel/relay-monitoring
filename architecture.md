# Relay-Monitoring Application — Architecture & Data Flow

## 1. High-Level System Architecture

```mermaid
graph TB
    subgraph PHYSICAL["⚡ Physical Layer"]
        R1["SEL-751 Relay<br/>192.168.0.2:23"]
        R2["Relay N...<br/>host:23"]
    end

    subgraph APP["🖥️ Relay-Monitoring Application (C++)"]

        subgraph INIT["Initialization (main.cpp)"]
            MAIN["main()"] --> LOGGER["AppLogger::init()<br/>5MB rotation, 3 backups"]
            MAIN --> APPSTART["TelnetSmlApp::start()"]
        end

        subgraph ORCHESTRATOR["TelnetSmlApp::Impl (Orchestrator)"]
            DB["SERDatabase<br/>SQLite WAL Mode<br/>ser_records.db"]
            WS["SERWebSocketServer<br/>localhost:8765"]
            SHM["SharedRingBuffer<br/>500KB MPMC"]
            TM["ThreadManager"]
            RM["RelayManager"]
            TARCACHE["TAR Cache<br/>(tarCacheMutex_)"]
        end

        subgraph POLLER["SER Poller Thread"]
            POLL["SERPoller<br/>Every 120 seconds"] -->|"queueCommand(all_relays, SER)"| RM
        end

        subgraph PIPELINE1["RelayPipeline (Per-Relay)"]
            TC["TelnetClient<br/>Boost.Asio TCP"]
            RB["RawDataRingBuffer<br/>100 messages MPMC"]

            subgraph RX["PipelineReceptionWorker Thread"]
                CFSM["RelayConnectionFSM<br/>(Boost.SML)"]
                CMDFSM["RelayCommandFSM<br/>(Boost.SML)"]
                CMDQ["Command Queue<br/>(async deque)"]
            end

            subgraph PROC["PipelineProcessingWorker Thread"]
                PARSER["SER Response Parser"]
                STAMP["Relay Identity Stamper<br/>relay_id + relay_name"]
                PRUNE["DB Pruner<br/>90 days, every 50 cycles"]
            end
        end

        subgraph TARBG["Background TAR Thread (Per-Relay)"]
            TARCOL["TAR Collector<br/>TAR 0..N until Invalid"]
        end
    end

    subgraph BROWSER["🌐 Browser (relay-app/)"]
        WORKER["ser_worker.js<br/>Web Worker"]
        MAINJS["sections.js<br/>Main Thread"]
        TAB["Tabulator Table<br/>SER Records Display"]
        TLV_DEC["TLV Decoder<br/>decodeSerRecordsFromTlv()"]
    end

    %% Physical → App
    R1 <-->|"TCP/Telnet"| TC
    R2 <-->|"TCP/Telnet"| TC

    %% Internal App Flow
    TM --> POLL
    RM -->|"startRelay() / stopRelay()"| PIPELINE1
    APPSTART --> DB & WS & TM & RM

    TC <-->|"SendCmdReceiveData()"| RX
    CFSM -->|"connect/login/retry"| TC
    CMDFSM -->|"SER/TAR/EVE cmds"| TC
    CMDQ -->|"queue_cv_ notify"| CFSM

    RX -->|"push({cmd, response})"| RB
    RB -->|"waitPop(readerId)"| PROC

    PARSER --> STAMP
    STAMP -->|"insertAndGetNewRecords()"| DB
    STAMP -->|"calls broadcastAll()"| WS
    STAMP -->|"write(tlv_payload)"| SHM
    PROC --> PRUNE -->|"pruneOldRecords(90)"| DB

    %% KEY: WebSocket reads ALL records from SQLite before broadcasting
    WS -->|"db_.getAllRecords()<br/>+ encodeSerRecordsToTlv()"| DB

    TARCOL -->|"handleUserCommand(TAR N)"| RM
    TARCOL -->|"cache result"| TARCACHE
    TARCOL -->|"broadcastText(batch)"| WS

    %% App → Browser
    WS <-->|"WebSocket :8765<br/>Binary TLV + Text"| WORKER
    WORKER -->|"postMessage()"| MAINJS
    MAINJS --> TLV_DEC --> TAB

    %% Browser → App Commands
    MAINJS -->|"relay_id:command"| WS
    WS -->|"cmdHandler / actionHandler"| RM

    style PHYSICAL fill:#fee2e2,stroke:#dc2626,color:#000
    style APP fill:#eff6ff,stroke:#2563eb,color:#000
    style BROWSER fill:#f0fdf4,stroke:#16a34a,color:#000
    style PIPELINE1 fill:#fef3c7,stroke:#d97706,color:#000
    style ORCHESTRATOR fill:#e0e7ff,stroke:#4f46e5,color:#000
    style POLLER fill:#fce7f3,stroke:#db2777,color:#000
    style TARBG fill:#f3e8ff,stroke:#9333ea,color:#000
```

---

## 2. Per-Relay Pipeline — Detailed Data Flow

```mermaid
flowchart TD
    subgraph RELAY["Physical Relay (TCP:23)"]
        DEV["SEL-751<br/>192.168.0.2"]
    end

    subgraph RX_THREAD["Reception Worker Thread"]
        Q["Command Queue<br/>(deque + cv)"]
        Q -->|"pop command"| EXEC

        subgraph FSM_DRIVE["driveToOperational()"]
            IDLE["Idle"] -->|"start_event"| CONNECTING
            CONNECTING -->|"ConnectAction<br/>TCP connect 2s timeout"| CHECK_CONN{Connect OK?}
            CHECK_CONN -->|"Yes"| LOGIN["Login_L1<br/>LoginLevel1Function()"]
            CHECK_CONN -->|"No"| CW["ConnectWait<br/>5s delay"] --> CONNECTING
            LOGIN -->|"Login1CompleteGuard"| OP["Operational ✅"]
            LOGIN -->|"InvalidLoginGuard<br/>CanRetry (max 3)"| LRW["LoginRetryWait<br/>5s delay"] --> CONNECTING
            LOGIN -->|"MaxRetriesReached"| ERR["Error State"]
            ERR -->|"60s auto-recovery<br/>disconnect_event"| IDLE
        end

        EXEC["executeCommand()"]
        EXEC -->|"driveToOperational()"| FSM_DRIVE
        OP --> FIRE["fireCommandEvent()"]

        FIRE -->|"SER"| SER_CMD["CmdSerAction<br/>SendCmdReceiveData('SER')"]
        FIRE -->|"TAR N"| TAR_CMD["CmdTarAction<br/>SendCmdReceiveData('TAR N')"]
        FIRE -->|"EVE"| EVE_CMD["SendCmdMultiPage('EVE')"]
        FIRE -->|"SET ..."| SET_CMD["CmdSetAction"]
        FIRE -->|"Other"| GEN_CMD["Generic Send"]

        SER_CMD & TAR_CMD & EVE_CMD & SET_CMD & GEN_CMD --> RESULT{Success?}
        RESULT -->|"Yes"| PUSH["rawBuffer_.push()<br/>{cmd, response}"]
        RESULT -->|"TIMEOUT"| RETRY["Retry (max 3)"] --> EXEC
        RESULT -->|"CONN_LOST"| RECONN["Reconnect + Retry"] --> FSM_DRIVE
    end

    subgraph RING["RawDataRingBuffer (100 msgs)"]
        BUF["MPMC Ring<br/>mutex + cv"]
    end

    subgraph PROC_THREAD["Processing Worker Thread"]
        WAIT["waitPop(readerId)"] --> CHECK_CMD{Is SER?}

        CHECK_CMD -->|"No"| BCAST_TEXT["wsServer.broadcastText()<br/>(raw response)"]

        CHECK_CMD -->|"Yes"| PARSE["parseSERResponse()"]
        PARSE --> STAMP_ID["Stamp relay_id<br/>+ relay_name"]
        STAMP_ID --> INSERT["db.insertAndGetNewRecords()<br/>UNIQUE(relay_id, record_id, timestamp)"]
        INSERT --> NEW{New records?}
        NEW -->|"Yes"| BCAST_ALL["wsServer.broadcastAll()<br/>TLV encoded"]
        NEW -->|"Yes"| SHM_WRITE["shmRing.write(tlv)"]
        INSERT --> PRUNE_CHK{Every 50 cycles?}
        PRUNE_CHK -->|"Yes"| PRUNE_DB["db.pruneOldRecords(90)"]
    end

    DEV <-->|"TCP/Telnet I/O"| EXEC
    PUSH --> BUF
    BUF --> WAIT

    style RELAY fill:#fee2e2,stroke:#dc2626,color:#000
    style RX_THREAD fill:#fef3c7,stroke:#d97706,color:#000
    style RING fill:#e0f2fe,stroke:#0284c7,color:#000
    style PROC_THREAD fill:#f0fdf4,stroke:#16a34a,color:#000
    style FSM_DRIVE fill:#fff7ed,stroke:#ea580c,color:#000
```

---

## 3. WebSocket Communication — Browser ↔ Server

```mermaid
sequenceDiagram
    participant B as Browser (sections.js)
    participant W as ser_worker.js (Web Worker)
    participant S as SERWebSocketServer (:8765)
    participant RM as RelayManager
    participant P as RelayPipeline
    participant DB as SERDatabase

    Note over W,S: Connection with Auto-Reconnect (3s → 30s backoff)

    %% SER Polling Cycle
    rect rgb(240, 253, 244)
        Note over S,P: SER Polling Cycle (every 120s)
        S->>RM: queueCommand(relay_id, "SER")
        RM->>P: rxWorker.command_queue_.push("SER")
        P->>P: TelnetClient → Relay → Response
        P->>P: Parse SER → Insert DB
        P->>DB: insertAndGetNewRecords()
        P->>S: broadcastAll() [Binary TLV]
        S->>W: WebSocket Binary Frame
        W->>B: postMessage(binary)
        B->>B: decodeSerRecordsFromTlv()
        B->>B: Update Tabulator Table
    end

    %% User Command
    rect rgb(254, 243, 199)
        Note over B,P: User Command (e.g., TAR 0)
        B->>W: send("1:TAR 0")
        W->>S: WebSocket Text Frame
        S->>RM: cmdHandler("1", "TAR 0")
        RM->>P: handleUserCommand("TAR 0")
        P->>P: TelnetClient.SendCmdReceiveData("TAR 0")
        P-->>S: Return response string
        S-->>W: WebSocket Text Frame
        W-->>B: postMessage(text)
        B->>B: Display TAR response
    end

    %% Relay Control
    rect rgb(243, 232, 255)
        Note over B,RM: Relay Lifecycle Control
        B->>S: JSON: {"action":"start_relay","relay_id":"2"}
        S->>RM: startRelay("2")
        RM->>RM: Create new RelayPipeline
        RM-->>S: Success response
        S-->>B: JSON confirmation
    end

    %% Background TAR
    rect rgb(252, 231, 243)
        Note over S,P: Background TAR Collection (on relay start)
        loop TAR 0, 1, 2, ... N
            P->>P: handleUserCommand("TAR " + i)
            P->>P: Accumulate responses
        end
        P->>S: broadcastText(TAR_BATCH_ALL)
        S->>W: WebSocket Text Frame
        W->>B: postMessage(tar_batch)
    end
```

---

## 4. Thread Architecture & Synchronization

```mermaid
graph LR
    subgraph THREADS["All Application Threads"]
        MAIN_T["🔵 Main Thread<br/>Control + waitForExit()"]
        POLL_T["🟣 SER Poller Thread<br/>120s interval"]
        WS_LISTEN["🟢 WS Listener Thread<br/>Accept connections"]
        WS_H1["🟢 WS Handler Thread 1"]
        WS_H2["🟢 WS Handler Thread N"]

        subgraph PER_RELAY["Per-Relay Threads (× N relays)"]
            RX_T["🟡 Reception Worker<br/>Telnet I/O + FSMs"]
            PROC_T["🟠 Processing Worker<br/>Parse + DB + Broadcast"]
            TAR_T["🟤 Background TAR<br/>(temporary)"]
        end
    end

    subgraph MUTEXES["Mutex Protection Map"]
        M1["🔒 RelayManager::mutex_<br/>→ active pipelines map"]
        M2["🔒 sync_cmd_mutex_<br/>→ Connection FSM + Cmd FSM"]
        M3["🔒 queue_mutex_<br/>→ command_queue_ deque"]
        M4["🔒 RingBuffer::mutex_<br/>→ queue_, readers_[]"]
        M5["🔒 tarCacheMutex_<br/>→ tarCache_, fetchInProgress"]
        M6["🔒 tarBgThreadsMutex_<br/>→ background thread vector"]
        M7["🔒 SessionManager::mutex_<br/>→ WS sessions set"]
        M8["🔒 AppLogger::mutex_<br/>→ log file rotation"]
    end

    MAIN_T -->|"uses"| M1
    POLL_T -->|"uses"| M1
    RX_T -->|"uses"| M2 & M3 & M4
    PROC_T -->|"uses"| M4
    TAR_T -->|"uses"| M5 & M6
    WS_H1 -->|"uses"| M1 & M7
    WS_H2 -->|"uses"| M1 & M7
    WS_LISTEN -->|"uses"| M7

    style THREADS fill:#f8fafc,stroke:#475569,color:#000
    style PER_RELAY fill:#fef3c7,stroke:#d97706,color:#000
    style MUTEXES fill:#fef2f2,stroke:#dc2626,color:#000
```

---

## 5. Error Recovery & Auto-Heal Flow

```mermaid
stateDiagram-v2
    [*] --> Idle

    state "Connection Lifecycle" as CL {
        Idle --> Connecting: start_event

        Connecting --> Login_L1: Connect OK ✅
        Connecting --> ConnectWait: Connect FAIL ❌

        ConnectWait --> Connecting: 5s delay<br/>(unlimited retries)

        Login_L1 --> Operational: Login OK ✅
        Login_L1 --> LoginRetryWait: Invalid Login<br/>(retries ≤ 3)
        Login_L1 --> Error: Max Retries<br/>Reached (3)

        LoginRetryWait --> Connecting: 5s delay

        Operational --> Idle: disconnect_event<br/>(connection lost)

        Error --> Idle: 60s auto-recovery<br/>disconnect_event
    end

    state "Command Execution" as CE {
        state "In Operational State" as InOp
        InOp --> ExecuteCmd: command from queue

        ExecuteCmd --> Success: Response OK
        ExecuteCmd --> RetryCmd: TIMEOUT<br/>(max 3 retries)
        ExecuteCmd --> Reconnect: CONN_LOST

        RetryCmd --> ExecuteCmd: retry
        Reconnect --> Connecting: driveToOperational()
        Success --> InOp: next command
    end

    state "Worker Thread Recovery" as WR {
        state "Thread Running" as TR
        TR --> ExceptionCaught: Any exception
        ExceptionCaught --> TR: 5s backoff<br/>auto-restart
    }

    note right of Error
        After 60s, automatically
        transitions back to Idle
        and retries the full
        connection cycle
    end note

    note right of ConnectWait
        Network down? Power off?
        Retries FOREVER until
        relay comes back online
    end note
```

---

## 6. Data Encoding — ASN.1 BER/TLV Binary Protocol

```mermaid
graph TD
    subgraph ENCODE["Server-Side Encoding (ws_server.hpp)"]
        RECORDS["vector&lt;SERRecord&gt;"] --> TLV_ENC["encodeSerRecordsToTlv()"]

        TLV_ENC --> TOP["0x61 APPLICATION 1<br/>(Constructed)"]
        TOP --> SEQ1["0x30 SEQUENCE<br/>Record 1"]
        TOP --> SEQ2["0x30 SEQUENCE<br/>Record 2"]
        TOP --> SEQN["0x30 SEQUENCE<br/>Record N"]

        SEQ1 --> T80["0x80 record_id"]
        SEQ1 --> T81["0x81 timestamp"]
        SEQ1 --> T82["0x82 status"]
        SEQ1 --> T83["0x83 description"]
        SEQ1 --> T84["0x84 relay_id"]
        SEQ1 --> T85["0x85 relay_name"]
    end

    subgraph DECODE["Browser-Side Decoding (sections.js)"]
        BIN["Binary WebSocket Frame"] --> READ_TLV["readTlv()"]
        READ_TLV --> READ_LEN["readLength()<br/>BER length parsing"]
        READ_LEN --> DEC["decodeSerRecordsFromTlv()"]
        DEC --> OBJ["JS Object Array:<br/>{recordId, timestamp,<br/>status, description,<br/>relayId, relayName}"]
        OBJ --> TABLE["Tabulator Table<br/>50 rows/page"]
    end

    TLV_ENC -->|"WebSocket<br/>Binary Frame"| BIN

    style ENCODE fill:#e0e7ff,stroke:#4f46e5,color:#000
    style DECODE fill:#f0fdf4,stroke:#16a34a,color:#000
```

---

## 7. WebSocket to UI — Detailed Data Flow

### 7A. Complete Pipeline: Server → Ring Buffer → Decode → Table

```mermaid
flowchart TD
    subgraph SERVER["C++ Server (ws_server.hpp)"]
        direction TB
        DBQUERY["db_.getAllRecords()<br/>SELECT * FROM ser_records<br/>ORDER BY timestamp DESC"]
        DBQUERY --> ENCODE["asn_tlv::encodeSerRecordsToTlv()"]

        subgraph TLV_STRUCT["Binary TLV Structure"]
            direction TB
            T61["0x61 APPLICATION 1 (Constructed)"]
            T61 --> T30a["0x30 SEQUENCE - Record 1"]
            T61 --> T30b["0x30 SEQUENCE - Record 2"]
            T61 --> T30n["0x30 SEQUENCE - Record N"]
            T30a --> F80["0x80 record_id"]
            T30a --> F81["0x81 timestamp"]
            T30a --> F82["0x82 status"]
            T30a --> F83["0x83 description"]
            T30a --> F84["0x84 relay_id"]
            T30a --> F85["0x85 relay_name"]
        end

        ENCODE --> TLV_STRUCT
        TLV_STRUCT --> BCAST["sessionMgr_.getSessions()<br/>→ session->sendBroadcast(payload)"]
        BCAST --> WSBINARY["WebSocket Binary Frame<br/>ws_.binary(true)<br/>ws_.async_write()"]
    end

    subgraph WORKER["ser_worker.js (Web Worker Thread)"]
        direction TB
        WS_RECV["ws.onmessage(event)"]
        WS_RECV --> TYPE_CHECK{"data instanceof<br/>ArrayBuffer?"}

        TYPE_CHECK -->|"Yes (Binary)"| RING_WRITE["ring.writePayload(<br/>new Uint8Array(data))"]
        TYPE_CHECK -->|"No (Text)"| POST_TEXT["postMessage(<br/>{type:'ws_text', data})"]

        subgraph RING_WRITE_DETAIL["Ring Buffer Write (ring_buffer.js)"]
            direction TB
            RW1["Load writePos from header[0]<br/>(Atomics.load)"]
            RW1 --> RW2["Check capacity:<br/>payload + 4 bytes header"]
            RW2 --> RW3{"Enough space?"}
            RW3 -->|"No"| RW4["dropOldestForSlowest()<br/>Evict oldest for slowest reader"]
            RW3 -->|"Yes"| RW5["Write 4-byte LE length<br/>+ payload bytes"]
            RW4 --> RW5
            RW5 --> RW6["Atomics.store(header, 0, newWritePos)"]
            RW6 --> RW7["Atomics.add(header, 1, 1)<br/>Increment signal counter"]
            RW7 --> RW8["Atomics.notify(header, 1, 1)<br/>Wake sleeping readers"]
        end

        RING_WRITE --> RING_WRITE_DETAIL
    end

    subgraph SHARED_MEM["SharedArrayBuffer (512 KB)"]
        direction LR
        HEAD["Header (44 bytes)<br/>Int32[0]: writePos<br/>Int32[1]: signal<br/>Int32[2]: readerCount<br/>Int32[3..10]: per-reader readPos"]
        DATA["Data Ring<br/>[4-byte len][payload][4-byte len][payload]..."]
    end

    subgraph MAIN_THREAD["sections.js (Main UI Thread)"]
        direction TB
        ATOMIC_WAIT["Atomics.waitAsync(header, 1, lastSignal)<br/>Zero-CPU wait until signal changes"]
        ATOMIC_WAIT --> WAKE["Signal changed → Wake up"]
        WAKE --> DRAIN["Drain loop:<br/>payload = serRing.readPayload(serReaderId)"]

        subgraph RING_READ_DETAIL["Ring Buffer Read (ring_buffer.js)"]
            direction TB
            RR1["Load readPos from header[3 + readerId]"]
            RR1 --> RR2["Load writePos from header[0]"]
            RR2 --> RR3{"readPos == writePos?"}
            RR3 -->|"Yes"| RR4["return null (no data)"]
            RR3 -->|"No"| RR5["Read 4-byte LE length at readPos"]
            RR5 --> RR6["Slice payload: data[pos+4 .. pos+4+len]"]
            RR6 --> RR7["Atomics.store(header, 3+readerId, newReadPos)"]
            RR7 --> RR8["return Uint8Array payload"]
        end

        DRAIN --> RING_READ_DETAIL

        RING_READ_DETAIL --> DECODE["decodeSerRecordsFromTlv(payload)"]

        subgraph DECODE_STEPS["TLV Decode Pipeline"]
            direction TB
            D1["Read top-level: tag=0x61"]
            D1 --> D2["Loop: Read each 0x30 SEQUENCE"]
            D2 --> D3["For each record, read fields:<br/>0x80→recordId, 0x81→timestamp<br/>0x82→status, 0x83→description<br/>0x84→relayId, 0x85→relayName"]
            D3 --> D4["TextDecoder.decode(bytes) → string"]
            D4 --> D5["Split timestamp → date + time"]
            D5 --> D6["Build JS object:<br/>{sno, date, time, element, state,<br/>relayId, relayName}"]
        end

        DECODE --> DECODE_STEPS

        DECODE_STEPS --> FILTER["Filter by current relay ID:<br/>record.relayId === relay.id"]
        FILTER --> UPDATE["updateSerTable(records)"]

        subgraph TABLE_UPDATE["Table Update Logic"]
            direction TB
            U1{"serTable exists?"}
            U1 -->|"No"| U2["initSerTable(data)<br/>Create new Tabulator"]
            U1 -->|"Yes"| U3["Deduplicate:<br/>newRows = data.filter(<br/>r => !serKnownSnos.has(r.sno))"]
            U3 --> U4{"newRows.length > 0?"}
            U4 -->|"Yes"| U5["serTable.blockRedraw()<br/>serTable.addData(newRows)<br/>serKnownSnos.add(each sno)<br/>serTable.restoreRedraw()"]
            U4 -->|"No"| U6["Skip (no new data)"]
        end

        UPDATE --> TABLE_UPDATE
        TABLE_UPDATE --> STATS["updateSerStats()<br/>Count: N | Updated: HH:MM:SS"]
    end

    subgraph UI["Browser DOM (#ser-table)"]
        direction TB
        TABULATOR["Tabulator Table"]

        subgraph COLUMNS["Columns"]
            direction LR
            C1["S.No<br/>(recordId)"]
            C2["Date"]
            C3["Time"]
            C4["Element<br/>(description)"]
            C5["State<br/>(status)"]
        end

        TABULATOR --> COLUMNS

        subgraph FORMATTER["serStatusFormatter()"]
            direction TB
            F1{"value?"}
            F1 -->|"Asserted"| F2["ser-status--assert<br/>Green badge"]
            F1 -->|"Deasserted"| F3["ser-status--deassert<br/>Red badge"]
            F1 -->|"Other"| F4["Plain text"]
        end

        C5 --> FORMATTER

        PAGINATION["Pagination: 50 rows/page<br/>Selectable: 10, 20, 50, 100"]
        SORT["Default sort: S.No DESC<br/>(newest first)"]
    end

    %% Connections between subgraphs
    WSBINARY -->|"TCP :8765"| WS_RECV
    RING_WRITE_DETAIL -->|"Shared Memory<br/>Zero-Copy"| SHARED_MEM
    SHARED_MEM -->|"Atomic Signal<br/>Wake Reader"| ATOMIC_WAIT
    TABLE_UPDATE --> TABULATOR

    style SERVER fill:#e0e7ff,stroke:#4f46e5,color:#000
    style WORKER fill:#fef3c7,stroke:#d97706,color:#000
    style SHARED_MEM fill:#fce7f3,stroke:#db2777,color:#000
    style MAIN_THREAD fill:#f0fdf4,stroke:#16a34a,color:#000
    style UI fill:#fff7ed,stroke:#ea580c,color:#000
    style TLV_STRUCT fill:#ede9fe,stroke:#7c3aed,color:#000
    style DECODE_STEPS fill:#ecfdf5,stroke:#059669,color:#000
    style TABLE_UPDATE fill:#fefce8,stroke:#ca8a04,color:#000
    style RING_WRITE_DETAIL fill:#fef9c3,stroke:#a16207,color:#000
    style RING_READ_DETAIL fill:#d1fae5,stroke:#047857,color:#000
```

---

### 7B. Fallback Path: Direct WebSocket (No SharedArrayBuffer)

```mermaid
flowchart LR
    subgraph DIRECT["Direct WebSocket Path (Fallback)"]
        direction TB
        WS_D["new WebSocket(url)<br/>binaryType = 'arraybuffer'"]
        WS_D --> ON_MSG{"ws.onmessage"}

        ON_MSG -->|"Binary"| DEC_D["decodeSerRecordsFromTlv()<br/>(same decoder)"]
        ON_MSG -->|"Text"| TEXT_D["Handle TAR batch /<br/>JSON response"]

        DEC_D --> FILTER_D["Filter by relay ID"]
        FILTER_D --> UPDATE_D["updateSerTable(records)"]
    end

    NOTE["Used when:<br/>- SharedArrayBuffer unavailable<br/>- Missing COOP/COEP headers<br/>- Browser doesn't support<br/>  Atomics.waitAsync"]

    style DIRECT fill:#fef2f2,stroke:#dc2626,color:#000
    style NOTE fill:#f8fafc,stroke:#94a3b8,color:#000
```

---

### 7C. Auto-Reconnect & Status Flow

```mermaid
sequenceDiagram
    participant UI as Browser UI
    participant W as ser_worker.js
    participant S as WS Server :8765

    Note over W,S: Initial Connection
    W->>S: new WebSocket("ws://host:8765")
    W->>UI: postMessage({type:'ws_status', status:'connecting'})
    UI->>UI: updateSerConnectionStatus('connecting')<br/>Show: Yellow "Connecting..."

    S-->>W: WebSocket Handshake OK
    W->>UI: postMessage({type:'ws_status', status:'connected'})
    UI->>UI: updateSerConnectionStatus('connected')<br/>Show: Green "Connected"
    Note over W: Reset backoff to 3s

    Note over W,S: Normal Operation
    loop Every broadcastAll()
        S->>W: Binary TLV frame
        W->>W: ring.writePayload(data)
        Note over UI: Atomics.waitAsync wakes
        UI->>UI: readPayload → decode → updateTable
    end

    Note over W,S: Connection Lost
    S--xW: Connection drops
    W->>UI: postMessage({type:'ws_status', status:'disconnected'})
    UI->>UI: updateSerConnectionStatus('disconnected')<br/>Show: Red "Disconnected"

    Note over W: Exponential Backoff Reconnect
    loop Retry with backoff (3s → 6s → 12s → ... → 30s max)
        W->>W: setTimeout(reconnectDelay)
        W->>S: new WebSocket("ws://host:8765")
        alt Connection Fails
            W->>W: backoff = Math.min(backoff * 2, 30000)
        else Connection Succeeds
            W->>UI: postMessage({type:'ws_status', status:'connected'})
            Note over W: Reset backoff to 3s
        end
    end
```

---

### 7D. User Command Flow: UI → Server → Relay → UI

```mermaid
flowchart TD
    subgraph BROWSER["Browser UI"]
        BTN["User clicks button<br/>(e.g., TAR 0, SER, Refresh)"]
        BTN --> PREFIX["_prefixCmd('TAR 0')<br/>→ '1:TAR 0'<br/>(relay_id : command)"]
    end

    subgraph WORKER_CMD["ser_worker.js"]
        PREFIX -->|"postMessage({type:'send',<br/>payload:'1:TAR 0'})"| WS_SEND["ws.send('1:TAR 0')"]
    end

    subgraph SERVER_CMD["C++ WebSocket Server"]
        WS_SEND -->|"TCP :8765"| ON_READ["WebSocketSession::on_read()"]
        ON_READ --> ROUTE{"Route message"}
        ROUTE -->|"id:command format"| CMD_H["cmdHandler_(<br/>relayId='1', cmd='TAR 0')"]
        ROUTE -->|"JSON with 'action'"| ACT_H["actionHandler_(<br/>{action, relay_id, ...})"]
        ROUTE -->|"FETCH_ALL_TAR"| STREAM_H["streamCmdHandler_(<br/>spawns handler thread)"]
    end

    subgraph RELAY_EXEC["Relay Pipeline"]
        CMD_H --> RM_CMD["relayMgr->handleUserCommand(<br/>'1', 'TAR 0')"]
        RM_CMD --> PIPE_CMD["pipeline->handleUserCommand(<br/>'TAR 0')"]
        PIPE_CMD --> RX_CMD["rxWorker->executeCommand(<br/>'TAR 0')"]
        RX_CMD --> TELNET["TelnetClient.SendCmdReceiveData(<br/>'TAR 0')"]
        TELNET <-->|"TCP :23"| RELAY_DEV["Physical Relay"]
    end

    subgraph RESPONSE["Response Path"]
        TELNET --> RESP["Response string"]
        RESP --> WS_RESP["session->sendTextResponse(<br/>response)"]
        WS_RESP -->|"WebSocket Text Frame"| WORKER_RECV["ws.onmessage(text)"]
        WORKER_RECV --> POST_RESP["postMessage({type:'ws_text',<br/>data: response})"]
        POST_RESP --> UI_DISPLAY["Display in UI<br/>(TAR table / text area)"]
    end

    style BROWSER fill:#fff7ed,stroke:#ea580c,color:#000
    style WORKER_CMD fill:#fef3c7,stroke:#d97706,color:#000
    style SERVER_CMD fill:#e0e7ff,stroke:#4f46e5,color:#000
    style RELAY_EXEC fill:#fef2f2,stroke:#dc2626,color:#000
    style RESPONSE fill:#f0fdf4,stroke:#16a34a,color:#000
```

---

### 7E. Data Type Summary

```mermaid
graph LR
    subgraph TYPES["Data Types at Each Stage"]
        direction TB

        A["C++ vector&lt;SERRecord&gt;<br/>{id, relay_id, relay_name,<br/>record_id, timestamp,<br/>status, description}"]
        -->|"encodeSerRecordsToTlv()"| B["Binary TLV (Uint8Array)<br/>0x61 → 0x30 → 0x80..0x85"]
        -->|"WebSocket Binary"| C["ArrayBuffer<br/>(raw bytes over network)"]
        -->|"ring.writePayload()"| D["SharedArrayBuffer<br/>[4-byte len][payload]<br/>(zero-copy shared memory)"]
        -->|"ring.readPayload()"| E["Uint8Array<br/>(view into shared memory)"]
        -->|"decodeSerRecordsFromTlv()"| F["JS Object Array<br/>[{sno, date, time,<br/>element, state,<br/>relayId, relayName}]"]
        -->|"updateSerTable()"| G["Tabulator Row Data<br/>{sno, date, time,<br/>element, state}<br/>+ CSS formatted status"]
    end

    style A fill:#e0e7ff,stroke:#4f46e5,color:#000
    style B fill:#ede9fe,stroke:#7c3aed,color:#000
    style C fill:#fce7f3,stroke:#db2777,color:#000
    style D fill:#fef3c7,stroke:#d97706,color:#000
    style E fill:#fef9c3,stroke:#a16207,color:#000
    style F fill:#d1fae5,stroke:#047857,color:#000
    style G fill:#fff7ed,stroke:#ea580c,color:#000
```

---

## 8. Complete System Lifecycle

```mermaid
flowchart TD
    START["main()"] --> LOG["AppLogger::init()<br/>5MB, 3 backups"]
    LOG --> DB_OPEN["SERDatabase::open()<br/>SQLite WAL mode"]
    DB_OPEN --> WS_START["WSServer::start()<br/>localhost:8765"]
    WS_START --> TM_START["ThreadManager::startAll()<br/>SERPoller @ 120s"]
    TM_START --> AUTO["Auto-start all configured relays"]

    AUTO --> |"For each relay"| RM_START["RelayManager::startRelay(id)"]
    RM_START --> PIPE["Create RelayPipeline<br/>TelnetClient + RingBuffer"]
    PIPE --> RX_START["Start ReceptionWorker thread"]
    PIPE --> PROC_START["Start ProcessingWorker thread"]
    PIPE --> TAR_BG["Spawn Background TAR thread"]
    PIPE --> Q_SER["Queue initial 'SER' command"]

    Q_SER --> RUNNING["✅ RUNNING 24/7"]

    RUNNING -->|"stdin or SIGTERM"| SHUTDOWN

    subgraph SHUTDOWN["Graceful Shutdown Sequence"]
        direction TB
        S1["1. app_running = false"] --> S2["2. ThreadManager::stopAll()<br/>(join poller)"]
        S2 --> S3["3. WSServer::stop()<br/>(join handler threads)"]
        S3 --> S4["4. Join background TAR threads"]
        S4 --> S5["5. RelayManager::stopAll()<br/>(stop all pipelines)"]
        S5 --> S6["6. SERDatabase::close()"]
    end

    S6 --> EXIT["Process Exit"]

    style START fill:#dbeafe,stroke:#2563eb,color:#000
    style RUNNING fill:#dcfce7,stroke:#16a34a,color:#000
    style SHUTDOWN fill:#fee2e2,stroke:#dc2626,color:#000
    style EXIT fill:#f3f4f6,stroke:#6b7280,color:#000
```
