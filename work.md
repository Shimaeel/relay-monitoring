# Telnet FSM Implementation - Detailed Explanation

## Architecture Overview

The process starts when the **Relay device** sends live data over the network, which is received by the **TelnetClient** written in C++. As soon as data is received, it is immediately pushed into a **C++ MPMC ring buffer** (struct-based) to ensure the reception path remains non-blocking and lightweight.

A separate **processing thread** continuously reads from this ring buffer and forwards the data to the **FSM (Finite State Machine)** for validation and business logic handling. After processing, the validated data is written to **SQLite** for persistence and **then** emitted through the **WebSocket server** for real-time browser updates (only after successful DB write).

On the browser side, the **WebSocket Client** receives the live messages. If additional parsing or heavy computation is required, a **JavaScript Worker** can handle that workload to avoid blocking the UI thread. The worker writes processed data into a **Shared Ring Buffer** (using SharedArrayBuffer), and the **main JavaScript thread** reads from this buffer to update the DOM (since only the main thread has DOM access).

Finally, the main thread updates the **Tabulator table** to reflect real-time changes.

### C++ Process Architecture

```
Relay Device
     ↓
TelnetClient Thread (C++)
     ↓
C++ MPMC Ring Buffer
     ↓
FSM / Processing Thread
     ↓
SQLite (Persistent Storage)
     ↓
WebSocket Server (After DB Write)
```

### Browser Architecture

```
WebSocket Client
     ↓
JS Worker (Optional)
     ↓
Main JS Thread (DOM Access)
     ↓
Tabulator / JSON Export
```

### Architecture Diagram (Mermaid)

```mermaid
flowchart TB
    subgraph CPP["C++ Process - Sequential Pipeline"]
        direction TB
        relay["🔌 Relay Device"]
        telnetThread["TelnetClient Thread\n(ReceptionWorker)"]
        spscRing["C++ MPMC Ring Buffer\n(RawDataRingBuffer)"]
        fsmThread["FSM / Processing Thread\n(ProcessingWorker)"]
        sqlite["SQLite\n(Persistent Storage)"]
        wsServer["WebSocket Server\n(After DB Write)"]
        
        relay -->|"TCP"| telnetThread
        telnetThread -->|"push"| spscRing
        spscRing -->|"waitPop"| fsmThread
        fsmThread -->|"insertRecords"| sqlite
        sqlite -->|"on success"| wsServer
    end

    subgraph Browser["Browser - Sequential Pipeline"]
        direction TB
        wsClient["WebSocket Client"]
        jsWorker["JS Worker\n(Optional)"]
        mainJS["Main JS Thread\n(DOM Access)"]
        tabulator["Tabulator / JSON Export"]
        
        wsClient -->|"onmessage"| jsWorker
        jsWorker -->|"postMessage"| mainJS
        mainJS -->|"render"| tabulator
    end

    wsServer -->|"WebSocket\nASN.1 BER/TLV"| wsClient
```

```
C++ PROCESS                                    BROWSER
┌─────────────────┐                           ┌─────────────────┐
│   Relay Device  │                           │ WebSocket Client│
│  192.168.0.2:23 │                           └────────┬────────┘
└────────┬────────┘                                    │
         │ TCP                                         ▼
         ▼                                    ┌─────────────────┐
┌─────────────────┐                           │   JS Worker     │
│ TelnetClient    │                           │   (Optional)    │
│ Thread (C++)    │                           └────────┬────────┘
└────────┬────────┘                                    │
         │ push                                        ▼
         ▼                                    ┌─────────────────┐
┌─────────────────┐                           │ Main JS Thread  │
│ C++ MPMC Ring   │                           │  (DOM Access)   │
│ Buffer          │                           └────────┬────────┘
└────────┬────────┘                                    │
         │ waitPop                                     ▼
         ▼                                    ┌─────────────────┐
┌─────────────────┐                           │   Tabulator /   │
│ FSM/Processing  │                           │   JSON Export   │
│ Thread          │                           └─────────────────┘
└────────┬────────┘
         │ insertRecords
         ▼
┌─────────────────┐
│     SQLite      │
│   (Persistent)  │
└────────┬────────┘
         │ on success
         ▼
┌─────────────────┐
│ WebSocket Server│──────────────────────────▶ (to Browser)
│ (After DB Write)│        ASN.1 BER/TLV
└─────────────────┘
```

---

## 1. main.cpp - Application Entry Point

```cpp
TelnetClient client;                    // Network communication object

ConnectionConfig conn{                  // Target device
    "192.168.0.2", 23,                 // SEL-735 relay IP:port
    std::chrono::milliseconds(2000)    // Connection timeout
};

LoginConfig creds{ "acc", "OTTER" };   // Credentials

sml::sm<TelnetFSM> fsm{ client, conn, creds };  // Create state machine
fsm.process_event(start_event{});               // Kick off FSM
```

**Main Loop (10 iterations):**
1. Print new responses (with deduplication)
2. Send `step_event` to FSM → triggers state transitions
3. Check for Error state → break if error
4. Sleep 200ms between steps

---

## 2. TelnetClient (client.hpp/cpp) - Network Layer

| Method | Purpose |
|--------|---------|
| `connectCheck()` | Async TCP connect with timeout |
| `SendCmdReceiveData()` | **Core function** - sends command, reads until prompt |
| `LoginLevel1Function()` | Sends username + password |
| `isResponseComplete()` | Checks for prompt or "SER Response Complete" |
| `endsWithPrompt()` | Looks for `>`, `#`, `$`, `:`, `?` in last 30 chars |

**SendCmdReceiveData Flow:**
```
1. Send: "command\r\n"
2. Loop: read chunks (512 bytes)
3. Check: isResponseComplete(buffer)?
   - Yes → return true
   - No → continue reading
4. Timeout after 5 seconds → return false
```

---

## 3. TelnetFSM (telnet_fsm.hpp) - State Machine

**States:**
```
Idle → Connecting → Login_L1 → Operational ⟷ Polling
                        │              │
                        └───▶ Error ◀──┘
```

**Transition Table:**

| Current State | Event | Guard | Next State |
|--------------|-------|-------|------------|
| *Idle | start_event | - | Connecting |
| Connecting | step_event | ConnectOkGuard | Login_L1 |
| Connecting | step_event | ConnectFailGuard | Error |
| Login_L1 | step_event | Login1CompleteGuard | Operational |
| Login_L1 | step_event | Login1FailGuard | Error |
| Operational | step_event | - | Polling |
| Polling | step_event | SerCompleteGuard | Operational |
| Polling | step_event | SerFailGuard | Error |

**Actions (on state entry):**
- `Connecting` → `ConnectAction` → calls `client.connectCheck()`
- `Login_L1` → `Login1Action` → calls `client.LoginLevel1Function()`
- `Polling` → `PollSerAction` → calls `client.SendCmdReceiveData("SER")`

**Guards (conditions to transition):**
- `ConnectOkGuard` → `client.getLastIoResult() == true`
- `Login1CompleteGuard` → IO success + response has prompt
- `SerCompleteGuard` → IO success + response has prompt or "SER Response Complete"

---

## 4. Execution Flow

```
Step 0: start_event → Idle→Connecting (connects to relay)
Step 1: step_event  → Connecting→Login_L1 (sends acc/OTTER)
Step 2: step_event  → Login_L1→Operational (login complete)
Step 3: step_event  → Operational→Polling (sends SER)
Step 4: step_event  → Polling→Operational (SER complete)
Step 5: step_event  → Operational→Polling (sends SER again)
...repeats Operational⟷Polling...
```

---

## 5. Key Design Patterns

| Pattern | Implementation |
|---------|---------------|
| **State Machine** | Boost.SML declarative transitions |
| **Dependency Injection** | FSM receives client, configs at construction |
| **Single Responsibility** | TelnetClient handles networking, FSM handles logic |
| **DRY** | `SendCmdReceiveData` reused by all commands |

---

## 6. Build Command

```powershell
g++ -std=c++17 -I third_party/sml/include -I C:\Development\Libraries\boost_1_90_0 main.cpp client.cpp -o telnet_fsm_test.exe -lws2_32
```

## 7. Run Command

```powershell
.\telnet_fsm_test.exe
```

---

## 8. C++ & JavaScript Combined Data Flow — Relay to UI

### Overview

The system follows a **pipeline architecture** where physical relay devices feed data through a multi-threaded C++ backend, across WebSocket channels, and into a JavaScript browser UI for real-time visualization.

### Full Data Flow Diagram (Mermaid)

```mermaid
flowchart TB
    %% ── Physical Layer ──
    subgraph RELAY["⚡ Physical Relay Devices"]
        direction LR
        r1["SEL-751\n192.168.0.2:23"]
        r2["SEL-451\n192.168.0.3:23"]
        rN["Relay N\nIP:23"]
    end

    %% ── C++ Backend ──
    subgraph CPP["🔧 C++ Backend Process"]
        direction TB

        subgraph CONN["Connection Layer"]
            rm["RelayManager\n(creates pipelines on demand)"]
            rp1["RelayPipeline #1"]
            rp2["RelayPipeline #2"]
        end

        subgraph PIPE["Per-Relay Pipeline (thread pool)"]
            tc["TelnetClient\n(Boost.Asio TCP)"]
            fsm["TelnetFSM — Boost.SML\nIdle → Connecting → Login\n→ Operational ⟷ Polling"]
            recvW["ReceptionWorker\n(thread: TCP recv loop)"]
            ring["RawDataRingBuffer\n(MPMC per-relay)"]
            procW["ProcessingWorker\n(thread: parse + dispatch)"]
            parser["SER Parser\ntext → SERRecord"]
        end

        subgraph STORE["Storage & Dispatch"]
            db["SERDatabase\n(SQLite)"]
            tlvEnc["ASN.1 BER/TLV\nEncoder"]
            shm["SharedRingBuffer\n(MPMC pub-sub)"]
            shmDB["SharedDBMemory\n(change notifications)"]
        end

        subgraph WS["WebSocket Servers"]
            ws8765["SERWebSocketServer\n:8765\n(binary TLV broadcast)"]
            ws8766["WSDBServer\n:8766\n(JSON DB operations)"]
        end

        rm --> rp1 & rp2
        rp1 & rp2 --> tc
        tc --> fsm
        fsm -->|"SER command"| recvW
        recvW -->|"push raw bytes"| ring
        ring -->|"waitPop"| procW
        procW --> parser
        parser -->|"insertRecords"| db
        db -->|"on success"| tlvEnc
        tlvEnc -->|"binary payload"| ws8765
        parser -->|"JSON output"| shm
        db -->|"change notify"| shmDB
        shmDB --> ws8766
        db -->|"query/exec"| ws8766
    end

    %% ── Network Transport ──
    subgraph NET["🌐 Network Transport"]
        direction LR
        wsBin["WebSocket Binary\nASN.1 BER/TLV\nPort 8765"]
        wsJSON["WebSocket JSON\nDB API\nPort 8766"]
    end

    %% ── JavaScript Browser UI ──
    subgraph JS["🖥️ JavaScript Browser UI"]
        direction TB

        subgraph DASH["relay-app/index.html — Dashboard"]
            dashJS["dashboard.js\n(relay card grid)"]
            combJS["combine_ser.js\n(TLV decoder + aggregator)"]
            dataJS["data.js\n(relay definitions)"]
        end

        subgraph DETAIL["relay-app/relay.html — Relay Detail"]
            relayJS["relay.js\n(header, sync time)"]
            secJS["sections.js\n(per-relay tables)"]
        end

        subgraph DBCLIENT["WebSocket DB Client Layer"]
            dbcJS["db_client.js\nDatabaseClient class\n(Promise-based API)"]
            fetchJS["db_fetch_all.js\n(lightweight getAll)"]
            incJS["db_incremental.js\n(delta polling)"]
            queryJS["db_query.js\n(SQL queries)"]
            schemaJS["db_schema.js\n(table metadata)"]
        end

        subgraph WORKER["Web Worker Layer"]
            shmW["shm_worker.js\n(WebSocket → SharedArrayBuffer)"]
            sab["SharedRingBuffer\n(SharedArrayBuffer\nMPMC in JS)"]
        end

        subgraph RENDER["Rendering"]
            tab["Tabulator.js\n(SER events table)"]
            cards["Relay Cards\n(status, info)"]
            csv["CSV Export"]
        end

        combJS -->|"decode TLV\ntags 0x80–0x85"| tab
        dashJS --> cards
        dbcJS --> tab
        fetchJS --> tab
        incJS --> tab
        shmW --> sab
        sab -->|"postMessage"| tab
        tab --> csv
    end

    %% ── Cross-boundary Connections ──
    r1 & r2 & rN -->|"TCP Telnet\nSER queries"| tc
    ws8765 -->|"binary TLV"| wsBin
    ws8766 -->|"JSON req/res"| wsJSON
    wsBin -->|"onmessage\n(ArrayBuffer)"| combJS
    wsBin -->|"onmessage"| shmW
    wsJSON -->|"onmessage\n(JSON)"| dbcJS

    %% ── UI Triggers Back to C++ ──
    dashJS -->|"start relay\n(WS :8766)"| ws8766
    relayJS -->|"sync time\n(WS :8766)"| ws8766
    ws8766 -->|"RelayManager\n::startRelay()"| rm
```

### Data Format Flow

```mermaid
flowchart LR
    subgraph FORMATS["Data Format Transformations"]
        direction TB
        raw["Raw Telnet Text\n#  45  02/14/22  12:47:19.970  SALARM  Asserted"]
        struct["C++ SERRecord struct\n{relay_id, record_id,\ntimestamp, status, description}"]
        sql["SQLite Row\nser_records table\n(indexed, deduplicated)"]
        tlv["ASN.1 BER/TLV Binary\n0x61 → 0x30 → 0x80..0x85"]
        json["JSON Object\n{sno, date, time,\nelement, state, relay}"]
        dom["DOM Table Row\nTabulator.js cell rendering"]

        raw -->|"SER Parser\n(regex + split)"| struct
        struct -->|"insertRecords()\n(bulk transaction)"| sql
        struct -->|"encodeSerRecords\nToTlv()"| tlv
        tlv -->|"JS _cserDecodeTlv()\n(ArrayBuffer parse)"| json
        sql -->|"db_client.js\ngetAll/query"| json
        json -->|"Tabulator\naddRow()"| dom
    end
```

### Communication Channels

| Channel | Port | Protocol | Direction | Payload | Used By |
|---------|------|----------|-----------|---------|---------|
| Telnet | 23 | TCP | C++ → Relay | SER text commands | TelnetClient |
| SER WebSocket | 8765 | WS Binary | C++ → Browser | ASN.1 BER/TLV records | combine_ser.js, shm_worker.js |
| DB WebSocket | 8766 | WS JSON | Browser ↔ C++ | getAll, query, exec, schema | db_client.js, dashboard.js |
| SharedRingBuffer | IPC | Shared Memory | C++ → C++ / JS Worker | Binary TLV / JSON | shm_worker.js |
| SharedDBMemory | IPC | Shared Memory | C++ → C++ | Change notifications | WSDBServer |

### ASN.1 TLV Binary Wire Format

```
┌──────────────────────────────────────────────────────────────┐
│ Tag: 0x61 (APPLICATION 1, constructed)                       │
│ Length: N bytes                                               │
│ ┌──────────────────────────────────────────────────────────┐ │
│ │ Tag: 0x30 (SEQUENCE) ─ Record #1                        │ │
│ │ ┌────────────────────────────────────────────────────┐   │ │
│ │ │ 0x80 │ len │ record_id   ("45")                    │   │ │
│ │ │ 0x81 │ len │ timestamp   ("02/14/22 12:47:19.970") │   │ │
│ │ │ 0x82 │ len │ status      ("Asserted")              │   │ │
│ │ │ 0x83 │ len │ description ("SALARM")                │   │ │
│ │ │ 0x84 │ len │ relay_id    ("1")                     │   │ │
│ │ │ 0x85 │ len │ relay_name  ("SEL-751")               │   │ │
│ │ └────────────────────────────────────────────────────┘   │ │
│ │ Tag: 0x30 (SEQUENCE) ─ Record #2 ...                    │ │
│ └──────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘
```

### End-to-End Sequence

```mermaid
sequenceDiagram
    participant Relay as Relay Device
    participant TC as TelnetClient (C++)
    participant FSM as TelnetFSM
    participant Ring as RawDataRingBuffer
    participant Proc as ProcessingWorker
    participant DB as SQLite
    participant WS85 as SER WS :8765
    participant WS86 as DB WS :8766
    participant CombJS as combine_ser.js
    participant DBClient as db_client.js
    participant Tab as Tabulator Table

    Note over Relay, Tab: Browser triggers relay connection
    DBClient->>WS86: {"action":"startRelay","id":"1"}
    WS86->>FSM: RelayManager::startRelay()
    FSM->>TC: connectCheck(192.168.0.2, 23)
    TC->>Relay: TCP connect
    Relay-->>TC: connected
    FSM->>TC: LoginLevel1Function("acc","OTTER")
    TC->>Relay: username + password
    Relay-->>TC: prompt (">")

    loop Operational ⟷ Polling
        FSM->>TC: SendCmdReceiveData("SER")
        TC->>Relay: "SER\r\n"
        Relay-->>TC: SER text response
        TC->>Ring: push(raw bytes)
        Ring->>Proc: waitPop()
        Proc->>Proc: SER Parser (text → SERRecord[])
        Proc->>DB: insertRecords(records)
        DB-->>Proc: success
        Proc->>WS85: encodeSerRecordsToTlv() → broadcast
        WS85-->>CombJS: binary TLV (ArrayBuffer)
        CombJS->>CombJS: _cserDecodeTlv() → JSON rows
        CombJS->>Tab: addData(rows)
        Tab->>Tab: render table rows
    end

    Note over DBClient, Tab: DB Client queries (parallel path)
    DBClient->>WS86: {"action":"getAll","table":"ser_records"}
    WS86->>DB: SELECT * FROM ser_records
    DB-->>WS86: rows
    WS86-->>DBClient: {"ok":true,"columns":[...],"rows":[...]}
    DBClient->>Tab: updateData(rows)
```
