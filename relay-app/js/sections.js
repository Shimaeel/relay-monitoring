/**
 * ============================================================
 *  sections.js — Section Controller for Relay Detail Page
 * ============================================================
 *
 *  Handles dynamic section loading from the right sidebar tabs.
 *  The SER section includes full WebSocket connectivity with
 *  ASN.1 BER/TLV decoding, Tabulator table, connection status,
 *  auto-refresh polling, and CSV export — matching the proven
 *  implementation from ui/index.html.
 *
 *  Dependencies: data.js, relay.js, Tabulator (CDN)
 */

// ============================================================
//  Current Relay Context (resolved from URL ?id=)
// ============================================================
let _currentRelayCtx = null;

function getCurrentRelay() {
  if (_currentRelayCtx) return _currentRelayCtx;
  const id = new URLSearchParams(window.location.search).get("id");
  if (id && typeof getRelayById === "function") {
    _currentRelayCtx = getRelayById(id);
  }
  return _currentRelayCtx;
}

// ============================================================
//  SER Module State
// ============================================================
let serTable           = null;   // Tabulator instance
let serWs              = null;   // WebSocket connection
let serAutoRefresh     = null;   // Polling interval handle
let serLastMessageAt   = 0;      // Timestamp of last WS message
let serDataReceived    = false;  // Whether any data has arrived
let serWorkerConnected = false;  // WebSocket connected flag

const textDecoder = new TextDecoder();

// ============================================================
//  ASN.1 BER / TLV Decoder  (same protocol as ws_server.hpp)
// ============================================================

function readLength(buffer, offset) {
  const first = buffer[offset];
  if ((first & 0x80) === 0) {
    return { length: first, nextOffset: offset + 1 };
  }
  const count = first & 0x7f;
  if (count === 0 || offset + count >= buffer.length) {
    throw new Error("Invalid BER length");
  }
  let value = 0;
  for (let i = 0; i < count; i++) {
    value = (value << 8) | buffer[offset + 1 + i];
  }
  return { length: value, nextOffset: offset + 1 + count };
}

function readTlv(buffer, offset) {
  const tag = buffer[offset];
  const constructed = (tag & 0x20) !== 0;
  const lengthInfo = readLength(buffer, offset + 1);
  const valueStart = lengthInfo.nextOffset;
  const tlvLen = (valueStart - offset) + lengthInfo.length;
  return {
    tag,
    constructed,
    valueStart,
    length: lengthInfo.length,
    nextOffset: valueStart + lengthInfo.length,
    tlvLen
  };
}

/**
 * Decode ASN.1 BER/TLV payload into an array of SER record objects.
 *
 * TLV layout (from ws_server.hpp / asn_tlv_codec.hpp):
 *   0x61 (APPLICATION 1, constructed) → contains 0x30 SEQUENCE records
 *     0x80 record_id  (string)
 *     0x81 timestamp  (string)
 *     0x82 status     (string)
 *     0x83 description(string)
 */
function decodeSerRecordsFromTlv(arrayBuffer) {
  const buffer = new Uint8Array(arrayBuffer);
  let offset = 0;
  const topLevel = readTlv(buffer, offset);

  if (topLevel.tag !== 0x61) {
    throw new Error("Unexpected TLV tag: 0x" + topLevel.tag.toString(16));
  }

  const records = [];
  offset = topLevel.valueStart;
  const end = topLevel.valueStart + topLevel.length;

  while (offset < end) {
    const recordTlv = readTlv(buffer, offset);
    if (recordTlv.tag !== 0x30) { offset = recordTlv.nextOffset; continue; }

    const fields = {};
    let rOff = recordTlv.valueStart;
    const rEnd = recordTlv.valueStart + recordTlv.length;

    while (rOff < rEnd) {
      const fTlv = readTlv(buffer, rOff);
      const valueBytes = buffer.slice(fTlv.valueStart, fTlv.valueStart + fTlv.length);
      const val = textDecoder.decode(valueBytes);

      if (fTlv.tag === 0x80) fields.recordId    = val;
      if (fTlv.tag === 0x81) fields.timestamp   = val;
      if (fTlv.tag === 0x82) fields.status      = val;
      if (fTlv.tag === 0x83) fields.description = val;
      rOff = fTlv.nextOffset;
    }

    const ts = fields.timestamp || "";
    let date = ts || "-";
    let time = "-";
    const sp = ts.indexOf(" ");
    if (sp !== -1) { date = ts.slice(0, sp); time = ts.slice(sp + 1); }

    const snoVal = Number.parseInt(fields.recordId, 10);
    records.push({
      sno:     Number.isNaN(snoVal) ? (fields.recordId || "-") : snoVal,
      date:    date,
      time:    time,
      element: fields.description || "-",
      state:   fields.status || ""
    });

    offset = recordTlv.nextOffset;
  }

  return records;
}

// ============================================================
//  WebSocket Helpers
// ============================================================

function buildSerWsUrl() {
  const host = document.getElementById("ser-ws-host");
  const port = document.getElementById("ser-ws-port");
  const h = host ? host.value.trim() : "localhost";
  const p = port ? port.value.trim() : "8765";
  return `ws://${h}:${p}`;
}

function updateSerConnectionStatus(status) {
  const el = document.getElementById("ser-conn-status");
  if (!el) return;
  const map = {
    connecting:   { text: "🟡 Connecting…",  color: "#d97706" },
    connected:    { text: "🟢 Connected",     color: "#16a34a" },
    synced:       { text: "🟢 Data Synced",   color: "#16a34a" },
    disconnected: { text: "🔴 Disconnected",  color: "#dc2626" },
    error:        { text: "🔴 Error",         color: "#dc2626" }
  };
  const info = map[status] || map.disconnected;
  el.textContent = info.text;
  el.style.color = info.color;

  // Update empty-state hint
  updateSerEmptyState(status);
}

function updateSerStats() {
  const countEl  = document.getElementById("ser-record-count");
  const updateEl = document.getElementById("ser-last-update");
  if (serTable && countEl) {
    countEl.textContent = serTable.getDataCount();
  }
  if (updateEl) updateEl.textContent = new Date().toLocaleTimeString();
}

/** Show / hide the empty-state banner based on connection + data */
function updateSerEmptyState(status) {
  const emptyEl = document.getElementById("ser-live-empty");
  const titleEl = document.getElementById("ser-empty-title");
  const hintEl  = document.getElementById("ser-empty-hint");
  if (!emptyEl) return;

  if (serDataReceived) {
    emptyEl.classList.add("is-hidden");
    return;
  }

  const map = {
    connecting:   { t: "Connecting to live data source",   h: "Waiting for the WebSocket server to accept the connection." },
    connected:    { t: "Connected — waiting for data",     h: "No SER records received from the relay yet." },
    synced:       { t: "Connected — waiting for data",     h: "No SER records received from the relay yet." },
    disconnected: { t: "No live data (disconnected)",      h: "Start the application or check the WebSocket host and port." },
    error:        { t: "No live data (error)",             h: "Check the server logs and WebSocket availability." }
  };
  const info = map[status] || map.disconnected;
  if (titleEl) titleEl.textContent = info.t;
  if (hintEl)  hintEl.textContent  = info.h;
  emptyEl.classList.remove("is-hidden");
}

// ============================================================
//  Tabulator Status Formatter
// ============================================================

function serStatusFormatter(cell) {
  const v = cell.getValue();
  if (!v) return `<span class="ser-status--empty">—</span>`;
  if (v === "Asserted") {
    return `<span class="ser-status--assert">${v}</span>`;
  }
  if (v === "Deasserted") {
    return `<span class="ser-status--deassert">${v}</span>`;
  }
  return `<span>${v}</span>`;
}

// ============================================================
//  SER Tabulator Initialisation
// ============================================================

function initSerTable(data) {
  serTable = new Tabulator("#ser-table", {
    data: data,
    layout: "fitColumns",
    pagination: true,
    paginationSize: 50,
    paginationSizeSelector: [10, 20, 50, 100],
    movableColumns: true,
    placeholder: "No SER Records Available",
    height: "500px",
    columns: [
      {
        title: "S.No",
        field: "sno",
        widthGrow: 0,
        width: 230,
        hozAlign: "center",
        headerSort: true,
        sorter: "number",
        headerFilter: "input"
      },
      {
        title: "Date",
        field: "date",
        widthGrow: 0,
        width: 230,
        hozAlign: "center",
        headerSort: true,
        headerFilter: "input"
      },
      {
        title: "Time",
        field: "time",
       
        width: 230,
        hozAlign: "center",
        headerSort: true,
        headerFilter: "input"
      },
      {
        title: "Element",
        field: "element",
        
        width: 230,
        headerSort: true,
        headerFilter: "input"
      },
      {
        title: "State",
        field: "state",
        
        width: 230,
        hozAlign: "center",
        formatter: serStatusFormatter,
        headerSort: true,
        headerFilter: "input"
      }
    ],
    initialSort: [{ column: "sno", dir: "desc" }]
  });
  updateSerStats();
}

function updateSerTable(data) {
  if (serTable) {
    serTable.setData(data);
  } else {
    initSerTable(data);
  }
  serDataReceived = true;
  updateSerStats();

  // Hide empty state once we have data
  const emptyEl = document.getElementById("ser-live-empty");
  if (emptyEl) emptyEl.classList.add("is-hidden");
}

// ============================================================
//  Direct WebSocket Connection
// ============================================================

function connectSerWebSocket() {
  disconnectSerWebSocket();
  updateSerConnectionStatus("connecting");

  const url = buildSerWsUrl();
  console.log("[SER] Connecting to", url);
  serWs = new WebSocket(url);
  serWs.binaryType = "arraybuffer";

  serWs.onopen = () => {
    serWorkerConnected = true;
    updateSerConnectionStatus("connected");
    startSerAutoRefresh();
    // Request an immediate snapshot to populate the table on connect
    try {
      serWs.send("getData");
    } catch (_) {
      // Ignore send errors during handshake edge cases
    }
    console.log("[SER] WebSocket connected");
  };

  serWs.onmessage = async (event) => {
    try {
      let bytes;

      // --- Blob ---
      if (event.data instanceof Blob) {
        console.log("[SER] Received Blob:", event.data.size, "bytes");
        const ab = await event.data.arrayBuffer();
        bytes = new Uint8Array(ab);
      }
      // --- ArrayBuffer ---
      else if (event.data instanceof ArrayBuffer) {
        console.log("[SER] Received ArrayBuffer:", event.data.byteLength, "bytes");
        bytes = new Uint8Array(event.data);
      }
      // --- Text / JSON ---
      else {
        console.log("[SER] Received text:", event.data);
        try {
          const json = JSON.parse(event.data);
          if (Array.isArray(json)) {
            updateSerTable(json);
            serLastMessageAt = Date.now();
          }
        } catch (_) { /* non-JSON text */ }
        return;
      }

      if (!bytes || bytes.length === 0) {
        console.warn("[SER] Empty binary payload");
        return;
      }

      console.log("[SER] Binary length:", bytes.length, "| First byte:", "0x" + bytes[0].toString(16));

      // Slice exact payload region (critical for correct TLV parsing)
      const exactBuffer = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
      const records = decodeSerRecordsFromTlv(exactBuffer);

      console.log("[SER] Decoded", records.length, "records");

      if (records.length > 0) {
        updateSerTable(records);
        serLastMessageAt = Date.now();
      }
    } catch (err) {
      console.error("[SER] Decode error:", err);
    }
  };

  serWs.onclose = () => {
    serWorkerConnected = false;
    console.log("[SER] WebSocket closed");
    updateSerConnectionStatus(serDataReceived ? "synced" : "disconnected");
    stopSerAutoRefresh();
    // Auto-reconnect after 3 s (only if SER section still visible)
    setTimeout(() => {
      if (!serWorkerConnected && document.getElementById("ser-table")) {
        connectSerWebSocket();
      }
    }, 3000);
  };

  serWs.onerror = (err) => {
    console.error("[SER] WebSocket error:", err);
    updateSerConnectionStatus("error");
  };
}

function disconnectSerWebSocket() {
  stopSerAutoRefresh();
  if (serWs) {
    serWs.onclose = null;
    serWs.onerror = null;
    serWs.close();
    serWs = null;
  }
  serWorkerConnected = false;
}

// ============================================================
//  Auto-Refresh Polling
// ============================================================

function startSerAutoRefresh() {
  const rateEl = document.getElementById("ser-poll-rate");
  const rate = rateEl ? (parseInt(rateEl.value, 10) || 2000) : 2000;
  stopSerAutoRefresh();
  serAutoRefresh = setInterval(() => {
    if (Date.now() - serLastMessageAt < rate) return;
    if (serWs && serWs.readyState === WebSocket.OPEN) {
      serWs.send("getData");
    }
  }, rate);
}

function stopSerAutoRefresh() {
  if (serAutoRefresh) { clearInterval(serAutoRefresh); serAutoRefresh = null; }
}

// ============================================================
//  SER Section Actions  (called from onclick in SER HTML)
// ============================================================

function serRefresh() {
  if (serWs && serWs.readyState === WebSocket.OPEN) {
    serWs.send("refresh");
  } else {
    connectSerWebSocket();
  }
}

function serExportCSV() {
  if (serTable) serTable.download("csv", "ser_records.csv");
}

function serSendQuit() {
  if (serWs && serWs.readyState === WebSocket.OPEN) {
    serWs.send("QUIT");
    serWs.close();
    stopSerAutoRefresh();
    updateSerConnectionStatus("disconnected");
  }
}

// ============================================================
//  RELAY WORD (TAR) Module
// ============================================================
//
//  Clean async/await implementation.
//  Sends TAR 0 → TAR 77 sequentially, waits for each response
//  before sending the next, and updates the Tabulator table
//  row-by-row.
//
//  Public API (called from onclick in relay.html):
//    rwFetchAll()   — start sequential fetch
//    rwExportCSV()  — download CSV
//    rwDisconnect() — abort & disconnect
// ============================================================

// ── State ───────────────────────────────────────────────────

let rwTable     = null;   // Tabulator instance (created once)
let rwWs        = null;   // WebSocket connection
let rwAbort     = false;  // Abort signal for in-flight fetch
const RW_TOTAL  = 78;     // TAR 0 through TAR 77

// ── Promise queue for request-response over WebSocket ───────

let _rwResolve  = null;   // resolve() of current pending sendCommand
let _rwReject   = null;   // reject()  of current pending sendCommand

// ── UI Helpers ──────────────────────────────────────────────

function _rwBuildWsUrl() {
  const h = (document.getElementById("rw-ws-host") || {}).value?.trim() || "localhost";
  const p = (document.getElementById("rw-ws-port") || {}).value?.trim() || "8765";
  return `ws://${h}:${p}`;
}

function _rwSetStatus(status) {
  const el = document.getElementById("rw-conn-status");
  if (!el) return;
  const map = {
    idle:         { text: "🟡 Idle",           color: "#d97706" },
    connecting:   { text: "🟡 Connecting…",    color: "#d97706" },
    fetching:     { text: "🔵 Fetching…",      color: "#2563eb" },
    done:         { text: "🟢 Done",           color: "#16a34a" },
    error:        { text: "🔴 Error",          color: "#dc2626" },
    disconnected: { text: "🔴 Disconnected",   color: "#dc2626" },
    aborted:      { text: "🟠 Aborted",        color: "#ea580c" }
  };
  const info = map[status] || map.idle;
  el.textContent = info.text;
  el.style.color = info.color;
}

function _rwSetProgress(current, total) {
  const container = document.getElementById("rw-progress-container");
  const bar       = document.getElementById("rw-progress-bar");
  const text      = document.getElementById("rw-progress-text");
  if (!container) return;

  if (current < 0) {                       // hide
    container.style.display = "none";
    return;
  }
  container.style.display = "";
  const pct = Math.min(100, Math.round((current / total) * 100));
  if (bar)  bar.style.width = pct + "%";
  if (text) text.textContent = `Fetching TAR ${current} / ${total}  (${pct}%)`;
}

function _rwSetRowCount(n) {
  const el = document.getElementById("rw-row-count");
  if (el) el.textContent = n;
}

function _rwSetFetchBtn(disabled) {
  const btn = document.getElementById("rw-fetch-btn");
  if (!btn) return;
  btn.disabled = disabled;
  btn.style.opacity = disabled ? "0.5" : "1";
  btn.style.pointerEvents = disabled ? "none" : "";
}

// ── TAR Response Parser ─────────────────────────────────────

/**
 * Parse the raw text response of a single TAR command.
 *
 * Expected SEL relay output:
 *   TAR n
 *    ROW  n ( dnpIndex ):
 *            7       6       5       4       3       2       1       0
 *        -----   -----   -----   -----   -----   -----   -----   -----
 *        LED6    LED5    LED4    LED3    LED2    LED1    *       ENABLE
 *            0       0       0       0       0       0       0       1
 *   =>
 *
 * Or compact format:
 *   TAR n
 *   LED6    LED5    LED4    LED3    LED2    LED1    *       ENABLE
 *   0       0       0       0       0       0       0       1
 */
function parseTarResponse(text) {
  const lines = text.split(/\r?\n/);

  // ── Extract targetRow from "TAR n" ──
  let targetRow = null;
  let dnpIndex  = "—";

  for (const line of lines) {
    const m = line.match(/^\s*TAR\s+(\d+)/i);
    if (m) { targetRow = parseInt(m[1], 10); break; }
  }

  // Override with "ROW n ( dnp ):" if present
  const rowM = text.match(/ROW\s+(\d+)\s*\(\s*(\d+)\s*\)/i);
  if (rowM) {
    targetRow = parseInt(rowM[1], 10);
    dnpIndex  = parseInt(rowM[2], 10);
  }

  if (targetRow === null) return null;

  // ── Find the values line: exactly 8 tokens, all '0' or '1' ──
  let labels = Array(8).fill("");
  let values = Array(8).fill(0);

  for (let i = 0; i < lines.length; i++) {
    const tok = lines[i].trim().split(/\s+/).filter(Boolean);
    if (tok.length !== 8 || !tok.every(t => t === "0" || t === "1")) continue;

    values = tok.map(Number);

    // Walk backwards to find the label line
    for (let j = i - 1; j >= 0; j--) {
      const lt = lines[j].trim().split(/\s+/).filter(Boolean);
      if (lt.length === 0) continue;
      if (lt.every(t => /^-+$/.test(t))) continue;                           // dash row
      if (lt.length === 8 && lt.join(" ") === "7 6 5 4 3 2 1 0") continue;   // column header
      if (/ROW/i.test(lines[j])) continue;
      if (/^\s*TAR\s+\d+/i.test(lines[j])) continue;
      labels = lt.length <= 8 ? Array(8 - lt.length).fill("").concat(lt) : lt.slice(0, 8);
      break;
    }
    break;
  }

  return { targetRow, dnpIndex, labels, values };
}

// ── Tabulator — Init (once) ─────────────────────────────────

function _rwBitFormatter(cell) {
  const v = cell.getValue();
  if (v === null || v === undefined)
    return '<span style="color:#9ca3af">—</span>';

  const label = v.label || "";
  const val   = v.value;
  const on    = val === 1;
  const bg    = on ? "background:#dcfce7;border-radius:4px;" : "";
  const clr   = on ? "#16a34a" : "#9ca3af";

  return `<div style="text-align:center;padding:2px 0;${bg}">
    <div style="font-size:.7em;color:#6b7280;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:90px" title="${label}">${label || "—"}</div>
    <div style="font-weight:700;color:${clr};font-size:1.1em">${val ?? "—"}</div>
  </div>`;
}

function initRwTable() {
  if (rwTable) return;                 // never recreate

  const bitCol = (title, field) => ({
    title, field,
    hozAlign: "center",
    formatter: _rwBitFormatter,
    headerSort: true,
    width: 100,
    sorter: (a, b) => ((a?.value ?? -1) - (b?.value ?? -1))
  });

  rwTable = new Tabulator("#rw-table", {
    data: [],                          // start empty — rows added dynamically
    index: "targetRow",
    layout: "fitColumns",
    height: "600px",
    pagination: false,
    movableColumns: true,
    placeholder: "No Relay Word Data — click Fetch All TAR",
    columns: [
      { title: "Target Row", field: "targetRow", hozAlign: "center", headerSort: true, sorter: "number", width: 200, frozen: true },
      { title: "DNP Index",  field: "dnpIndex",  hozAlign: "center", headerSort: true, sorter: "number", width: 200 },
      bitCol("7", "bit7"),
      bitCol("6", "bit6"),
      bitCol("5", "bit5"),
      bitCol("4", "bit4"),
      bitCol("3", "bit3"),
      bitCol("2", "bit2"),
      bitCol("1", "bit1"),
      bitCol("0", "bit0")
    ]
  });
}

function _rwDnpRange(row) {
  const start = row * 8;
  const end = start + 7;
  return `${start}-${end}`;
}

// ── Tabulator — Update one row ──────────────────────────────

function _rwUpdateRow(parsed) {
  if (!rwTable) return;
  rwTable.updateOrAddData([{
    targetRow: parsed.targetRow,
    dnpIndex:  _rwDnpRange(parsed.targetRow),
    bit7: { label: parsed.labels[0], value: parsed.values[0] },
    bit6: { label: parsed.labels[1], value: parsed.values[1] },
    bit5: { label: parsed.labels[2], value: parsed.values[2] },
    bit4: { label: parsed.labels[3], value: parsed.values[3] },
    bit3: { label: parsed.labels[4], value: parsed.values[4] },
    bit2: { label: parsed.labels[5], value: parsed.values[5] },
    bit1: { label: parsed.labels[6], value: parsed.values[6] },
    bit0: { label: parsed.labels[7], value: parsed.values[7] }
  }]);
}

// ── WebSocket — Connect (returns Promise) ───────────────────

function _rwEnsureConnection() {
  return new Promise((resolve, reject) => {
    // Already open
    if (rwWs && rwWs.readyState === WebSocket.OPEN) {
      return resolve(rwWs);
    }

    // Clean up any previous socket
    _rwDisconnectRaw();

    _rwSetStatus("connecting");
    const url = _rwBuildWsUrl();
    console.log("[RW] Connecting to", url);
    rwWs = new WebSocket(url);

    rwWs.onopen = () => {
      console.log("[RW] Connected");
      resolve(rwWs);
    };

    rwWs.onerror = (err) => {
      console.error("[RW] Connection error:", err);
      reject(new Error("WebSocket connection failed"));
    };

    rwWs.onclose = () => {
      console.log("[RW] Closed");
      // If a sendCommand promise is pending, reject it
      if (_rwReject) {
        _rwReject(new Error("WebSocket closed unexpectedly"));
        _rwResolve = null;
        _rwReject  = null;
      }
    };

    // Route incoming text messages to the pending sendCommand promise
    rwWs.onmessage = (event) => {
      if (typeof event.data !== "string") return;
      if (_rwResolve) {
        const res = _rwResolve;
        _rwResolve = null;
        _rwReject  = null;
        res(event.data);             // resolve sendCommand()'s promise
      }
    };
  });
}

// ── WebSocket — Send one command, wait for response ─────────

/**
 * Send a single command string over WebSocket and wait for the
 * next text response.  Returns a Promise<string>.
 *
 * @param {string} cmd  e.g. "TAR 0"
 * @returns {Promise<string>}  raw relay response text
 */
async function sendCommand(cmd) {
  const ws = await _rwEnsureConnection();

  if (ws.readyState !== WebSocket.OPEN) {
    throw new Error("WebSocket is not open");
  }

  return new Promise((resolve, reject) => {
    // Set up the response listener (onmessage will call resolve)
    _rwResolve = resolve;
    _rwReject  = reject;

    // Timeout: 30 s per command (relay may be slow)
    const timer = setTimeout(() => {
      _rwResolve = null;
      _rwReject  = null;
      reject(new Error(`Timeout waiting for response to "${cmd}"`));
    }, 30_000);

    // Wrap resolve/reject to clear the timer
    const origResolve = resolve;
    const origReject  = reject;
    _rwResolve = (data) => { clearTimeout(timer); origResolve(data); };
    _rwReject  = (err)  => { clearTimeout(timer); origReject(err);   };

    console.log("[RW] Sending:", cmd);
    ws.send(cmd);
  });
}

// ── Core — fetchAllTAR() ────────────────────────────────────

/**
 * Sequentially sends TAR 0 through TAR 77, waits for each
 * response, parses it, and updates the Tabulator table row-by-row.
 */
async function fetchAllTAR() {
  // Prevent double invocation
  _rwSetFetchBtn(true);
  rwAbort = false;

  let completed = 0;

  try {
    // 1. Connect
    await _rwEnsureConnection();
    _rwSetStatus("fetching");
    _rwSetProgress(0, RW_TOTAL);
    _rwSetRowCount(0);

    // 2. Sequential loop
    for (let i = 0; i < RW_TOTAL; i++) {
      if (rwAbort) {
        console.log("[RW] Aborted by user at TAR", i);
        _rwSetStatus("aborted");
        break;
      }

      _rwSetProgress(i, RW_TOTAL);

      // Send command & wait for response
      const cmd      = "TAR " + i;
      const response = await sendCommand(cmd);

      // Parse
      const parsed = parseTarResponse(response);
      if (!parsed) {
        console.warn(`[RW] Failed to parse response for ${cmd}:`, response.substring(0, 200));
        throw new Error(`Failed to parse response for ${cmd}`);
      }

      // Update table
      _rwUpdateRow(parsed);
      completed++;
      _rwSetRowCount(completed);
      console.log(`[RW] TAR ${i} OK — row ${parsed.targetRow}, DNP ${parsed.dnpIndex}`);
    }

    // 3. Finished
    if (!rwAbort) {
      _rwSetProgress(-1);              // hide progress bar
      _rwSetStatus("done");
      console.log(`[RW] All ${RW_TOTAL} TAR commands completed`);
    }

  } catch (err) {
    console.error("[RW] fetchAllTAR error:", err);
    _rwSetStatus("error");
    _rwSetProgress(-1);

    // Show error toast if available
    if (typeof showToast === "function") {
      showToast(`TAR fetch failed at row ${completed}: ${err.message}`, "error");
    }
  } finally {
    _rwSetFetchBtn(false);
  }
}

// ── Disconnect helpers ──────────────────────────────────────

function _rwDisconnectRaw() {
  if (rwWs) {
    rwWs.onopen    = null;
    rwWs.onclose   = null;
    rwWs.onerror   = null;
    rwWs.onmessage = null;
    try { rwWs.close(); } catch (_) {}
    rwWs = null;
  }
  _rwResolve = null;
  _rwReject  = null;
}

// ── Public Actions (called from onclick in relay.html) ──────

function rwFetchAll() {
  fetchAllTAR();                       // fire-and-forget (async)
}

function rwExportCSV() {
  if (rwTable) rwTable.download("csv", "relay_word_targets.csv");
}

function rwDisconnect() {
  rwAbort = true;                      // signal fetchAllTAR to stop
  _rwDisconnectRaw();
  _rwSetStatus("idle");
  _rwSetProgress(-1);
  _rwSetFetchBtn(false);
}

// ============================================================
//  INPUT / OUTPUT (I/O) Module
// ============================================================
//
//  Fetches all TAR rows (0–77) just like the Relay Word module,
//  but keeps only rows whose bit labels start with "IN" or "OUT".
//  Displays them in a flattened Tabulator table with columns:
//    Target Row | DNP Index | Bit | Label | Value | Type
//
//  Public API (called from onclick in relay.html):
//    ioFetchAll()    — start fetch, filter, display
//    ioExportCSV()   — download CSV
//    ioDisconnect()  — abort & disconnect
//    ioSetFilter(f)  — filter by 'all' | 'in' | 'out'
// ============================================================

// ── State ───────────────────────────────────────────────────

let ioTable      = null;   // Tabulator instance
let ioWs         = null;   // WebSocket connection
let ioAbort      = false;  // Abort signal
let _ioAllRows   = [];     // All collected I/O rows (unfiltered)
let _ioFilter    = "all";  // Current filter: 'all' | 'in' | 'out'
const IO_TOTAL   = 78;     // TAR 0 through TAR 77

let _ioResolve   = null;
let _ioReject    = null;

// ── UI Helpers ──────────────────────────────────────────────

function _ioBuildWsUrl() {
  const h = (document.getElementById("io-ws-host") || {}).value?.trim() || "localhost";
  const p = (document.getElementById("io-ws-port") || {}).value?.trim() || "8765";
  return `ws://${h}:${p}`;
}

function _ioSetStatus(status) {
  const el = document.getElementById("io-conn-status");
  if (!el) return;
  const map = {
    idle:         { text: "🟡 Idle",           color: "#d97706" },
    connecting:   { text: "🟡 Connecting…",    color: "#d97706" },
    fetching:     { text: "🔵 Fetching…",      color: "#2563eb" },
    done:         { text: "🟢 Done",           color: "#16a34a" },
    error:        { text: "🔴 Error",          color: "#dc2626" },
    disconnected: { text: "🔴 Disconnected",   color: "#dc2626" },
    aborted:      { text: "🟠 Aborted",        color: "#ea580c" }
  };
  const info = map[status] || map.idle;
  el.textContent = info.text;
  el.style.color = info.color;
}

function _ioSetProgress(current, total) {
  const container = document.getElementById("io-progress-container");
  const bar       = document.getElementById("io-progress-bar");
  const text      = document.getElementById("io-progress-text");
  if (!container) return;

  if (current < 0) {
    container.style.display = "none";
    return;
  }
  container.style.display = "";
  const pct = Math.min(100, Math.round((current / total) * 100));
  if (bar)  bar.style.width = pct + "%";
  if (text) text.textContent = `Scanning TAR ${current} / ${total}  (${pct}%)`;
}

function _ioSetRowCount(n) {
  const el = document.getElementById("io-row-count");
  if (el) el.textContent = n;
}

function _ioSetFetchBtn(disabled) {
  const btn = document.getElementById("io-fetch-btn");
  if (!btn) return;
  btn.disabled = disabled;
  btn.style.opacity = disabled ? "0.5" : "1";
  btn.style.pointerEvents = disabled ? "none" : "";
}

// ── Tabulator — IO Status Formatter ─────────────────────────

function _ioValueFormatter(cell) {
  const v = cell.getValue();
  if (v === null || v === undefined) return '<span style="color:#9ca3af">—</span>';
  const on = v === 1;
  const bg  = on ? "background:#dcfce7;border-radius:4px;padding:2px 10px;" : "padding:2px 10px;";
  const clr = on ? "#16a34a" : "#9ca3af";
  const txt = on ? "1 (ON)" : "0 (OFF)";
  return `<span style="font-weight:700;color:${clr};${bg}">${txt}</span>`;
}

function _ioTypeFormatter(cell) {
  const v = cell.getValue();
  if (v === "IN") {
    return `<span style="color:#2563eb;font-weight:600;">⬇ Input</span>`;
  }
  if (v === "OUT") {
    return `<span style="color:#ea580c;font-weight:600;">⬆ Output</span>`;
  }
  return `<span>${v || "—"}</span>`;
}

// ── Tabulator — Init ────────────────────────────────────────

function initIoTable() {
  if (ioTable) return;

  ioTable = new Tabulator("#io-table", {
    data: [],
    layout: "fitColumns",
    height: "600px",
    pagination: true,
    paginationSize: 50,
    paginationSizeSelector: [10, 20, 50, 100],
    movableColumns: true,
    placeholder: "No I/O Data — click Fetch I/O to scan relay",
    columns: [
      { title: "Type",        field: "type",      hozAlign: "center", formatter: _ioTypeFormatter, headerSort: true, headerFilter: "input", width: 120 },
      { title: "Label",       field: "label",     hozAlign: "left",   headerSort: true, headerFilter: "input", width: 160 },
      { title: "Value",       field: "value",     hozAlign: "center", formatter: _ioValueFormatter, headerSort: true, headerFilter: "input", width: 120 },
      { title: "Target Row",  field: "targetRow", hozAlign: "center", headerSort: true, sorter: "number", headerFilter: "input", width: 120 },
      { title: "DNP Index",   field: "dnpIndex",  hozAlign: "center", headerSort: true, sorter: "number", headerFilter: "input", width: 120 },
      { title: "Bit",         field: "bit",       hozAlign: "center", headerSort: true, sorter: "number", headerFilter: "input", width: 80 }
    ],
    initialSort: [{ column: "type", dir: "asc" }, { column: "label", dir: "asc" }]
  });
}

// ── Tabulator — Filter ──────────────────────────────────────

function ioSetFilter(f) {
  _ioFilter = f;

  // Update button styles
  document.querySelectorAll(".io-filter-btn").forEach(btn => {
    btn.classList.remove("active");
    btn.classList.replace("btn--primary", "btn--outline");
  });
  const activeBtn = document.getElementById("io-filter-" + f);
  if (activeBtn) {
    activeBtn.classList.add("active");
    activeBtn.classList.replace("btn--outline", "btn--primary");
  }

  _ioApplyFilter();
}

function _ioApplyFilter() {
  if (!ioTable) return;
  let data = _ioAllRows;
  if (_ioFilter === "in")  data = _ioAllRows.filter(r => r.type === "IN");
  if (_ioFilter === "out") data = _ioAllRows.filter(r => r.type === "OUT");
  ioTable.setData(data);
  _ioSetRowCount(data.length);
}

// ── WebSocket — Connect ─────────────────────────────────────

function _ioEnsureConnection() {
  return new Promise((resolve, reject) => {
    if (ioWs && ioWs.readyState === WebSocket.OPEN) return resolve(ioWs);

    _ioDisconnectRaw();
    _ioSetStatus("connecting");
    const url = _ioBuildWsUrl();
    console.log("[IO] Connecting to", url);
    ioWs = new WebSocket(url);

    ioWs.onopen = () => { console.log("[IO] Connected"); resolve(ioWs); };
    ioWs.onerror = (err) => { console.error("[IO] Connection error:", err); reject(new Error("WebSocket connection failed")); };
    ioWs.onclose = () => {
      console.log("[IO] Closed");
      if (_ioReject) { _ioReject(new Error("WebSocket closed unexpectedly")); _ioResolve = null; _ioReject = null; }
    };
    ioWs.onmessage = (event) => {
      if (typeof event.data !== "string") return;
      if (_ioResolve) { const res = _ioResolve; _ioResolve = null; _ioReject = null; res(event.data); }
    };
  });
}

// ── WebSocket — Send command, wait for response ─────────────

async function _ioSendCommand(cmd) {
  const ws = await _ioEnsureConnection();
  if (ws.readyState !== WebSocket.OPEN) throw new Error("WebSocket is not open");

  return new Promise((resolve, reject) => {
    _ioResolve = resolve;
    _ioReject  = reject;

    const timer = setTimeout(() => {
      _ioResolve = null; _ioReject = null;
      reject(new Error(`Timeout waiting for response to "${cmd}"`));
    }, 30_000);

    const origResolve = resolve;
    const origReject  = reject;
    _ioResolve = (data) => { clearTimeout(timer); origResolve(data); };
    _ioReject  = (err)  => { clearTimeout(timer); origReject(err);   };

    console.log("[IO] Sending:", cmd);
    ws.send(cmd);
  });
}

// ── Core — fetchAllIO() ─────────────────────────────────────

/**
 * Fetch all TAR rows, collect only bits whose labels start with
 * "IN" or "OUT", build a flat table of individual I/O points.
 */
async function fetchAllIO() {
  _ioSetFetchBtn(true);
  ioAbort = false;
  _ioAllRows = [];

  try {
    await _ioEnsureConnection();
    _ioSetStatus("fetching");
    _ioSetProgress(0, IO_TOTAL);
    _ioSetRowCount(0);

    for (let i = 0; i < IO_TOTAL; i++) {
      if (ioAbort) {
        console.log("[IO] Aborted by user at TAR", i);
        _ioSetStatus("aborted");
        break;
      }

      _ioSetProgress(i, IO_TOTAL);

      const cmd      = "TAR " + i;
      const response = await _ioSendCommand(cmd);
      const parsed   = parseTarResponse(response);

      if (!parsed) {
        console.warn(`[IO] Failed to parse response for ${cmd}`);
        continue;   // skip unparseable rows instead of throwing
      }

      // Extract individual bits that are IN or OUT
      for (let b = 0; b < 8; b++) {
        const lbl = (parsed.labels[b] || "").trim();
        if (!lbl) continue;
        const upper = lbl.toUpperCase();
        let type = null;
        if (upper.startsWith("OUT")) type = "OUT";
        else if (upper.startsWith("IN"))  type = "IN";
        if (!type) continue;

        _ioAllRows.push({
          type:      type,
          label:     lbl,
          value:     parsed.values[b],
          targetRow: parsed.targetRow,
          dnpIndex:  parsed.dnpIndex,
          bit:       7 - b       // labels[0] = bit 7, labels[7] = bit 0
        });
      }

      // Live-update table as rows arrive
      _ioApplyFilter();
      console.log(`[IO] TAR ${i} scanned — ${_ioAllRows.length} I/O points so far`);
    }

    if (!ioAbort) {
      _ioSetProgress(-1);
      _ioSetStatus("done");
      _ioApplyFilter();
      console.log(`[IO] Scan complete — ${_ioAllRows.length} I/O points found`);
    }

  } catch (err) {
    console.error("[IO] fetchAllIO error:", err);
    _ioSetStatus("error");
    _ioSetProgress(-1);
    if (typeof showToast === "function") {
      showToast(`I/O fetch failed: ${err.message}`, "error");
    }
  } finally {
    _ioSetFetchBtn(false);
  }
}

// ── Disconnect helpers ──────────────────────────────────────

function _ioDisconnectRaw() {
  if (ioWs) {
    ioWs.onopen = null; ioWs.onclose = null; ioWs.onerror = null; ioWs.onmessage = null;
    try { ioWs.close(); } catch (_) {}
    ioWs = null;
  }
  _ioResolve = null;
  _ioReject  = null;
}

// ── Public Actions ──────────────────────────────────────────

function ioFetchAll() {
  fetchAllIO();   // fire-and-forget (async)
}

function ioExportCSV() {
  if (ioTable) ioTable.download("csv", "relay_io_points.csv");
}

function ioDisconnect() {
  ioAbort = true;
  _ioDisconnectRaw();
  _ioSetStatus("idle");
  _ioSetProgress(-1);
  _ioSetFetchBtn(false);
}

// ============================================================
//  Section Loader
// ============================================================

document.addEventListener("DOMContentLoaded", function () {

  const tabs      = document.querySelectorAll(".vertical-item");
  const container = document.getElementById("dynamic-section");

  // --- Resolve defaults (match ui/index.html: localhost:8765) ---
  const relay = getCurrentRelay();
  const defaultHost = "localhost";
  const defaultPort = 8765;

  // --- Pre-fill header config fields from relay data ---
  if (relay) {
    const setVal = (id, v) => { const el = document.getElementById(id); if (el && v) el.value = v; };
    setVal("cfg-substation", relay.substation);
    setVal("cfg-bay",        relay.bay);
    setVal("cfg-pse",        relay.pse);
    setVal("cfg-breaker",    relay.breaker);
    setVal("cfg-ip",         relay.ip);
  }

  function teardownSer() {
    disconnectSerWebSocket();
    // Keep table alive — just disconnect WS; redraw on re-show
    serDataReceived = false;
  }

  function teardownRelayWord() {
    rwAbort = true;
    _rwDisconnectRaw();
  }

  function teardownIO() {
    ioAbort = true;
    _ioDisconnectRaw();
  }

  // --- References to SER section (static HTML in relay.html) ---
  const serSection   = document.getElementById("ser-section");
  const serHostInput  = document.getElementById("ser-ws-host");
  const serPortInput  = document.getElementById("ser-ws-port");
  const serPollInput  = document.getElementById("ser-poll-rate");

  // --- References to Relay Word section (static HTML in relay.html) ---
  const rwSection    = document.getElementById("relayword-section");
  const rwHostInput  = document.getElementById("rw-ws-host");
  const rwPortInput  = document.getElementById("rw-ws-port");

  // --- References to IO section (static HTML in relay.html) ---
  const ioSection    = document.getElementById("io-section");
  const ioHostInput  = document.getElementById("io-ws-host");
  const ioPortInput  = document.getElementById("io-ws-port");

  // Pre-fill SER config inputs from relay context
  if (serHostInput) serHostInput.value = defaultHost;
  if (serPortInput) serPortInput.value = defaultPort;

  // Pre-fill Relay Word config inputs from relay context
  if (rwHostInput) rwHostInput.value = defaultHost;
  if (rwPortInput) rwPortInput.value = defaultPort;

  // Pre-fill IO config inputs from relay context
  if (ioHostInput) ioHostInput.value = defaultHost;
  if (ioPortInput) ioPortInput.value = defaultPort;

  // Poll-rate live update
  if (serPollInput) {
    serPollInput.addEventListener("change", () => {
      if (serWorkerConnected) startSerAutoRefresh();
    });
  }

  function showSerSection() {
    if (serSection) serSection.style.display = "";
    container.style.display = "none";

    // Wait one frame so the browser reflows and #ser-table has real dimensions
    requestAnimationFrame(() => {
      if (!serTable) {
        initSerTable([]);
      } else {
        // Force full redraw after becoming visible again
        serTable.redraw(true);
      }
      // Auto-connect if not already connected
      if (!serWs || serWs.readyState !== WebSocket.OPEN) connectSerWebSocket();
    });
  }

  function hideSerSection() {
    if (serSection) serSection.style.display = "none";
    container.style.display = "";
  }

  function showRelayWordSection() {
    if (rwSection) rwSection.style.display = "";
    container.style.display = "none";

    requestAnimationFrame(() => {
      if (!rwTable) {
        initRwTable();
      } else {
        rwTable.redraw(true);
      }
    });
  }

  function hideRelayWordSection() {
    if (rwSection) rwSection.style.display = "none";
    container.style.display = "";
  }

  function showIOSection() {
    if (ioSection) ioSection.style.display = "";
    container.style.display = "none";

    requestAnimationFrame(() => {
      if (!ioTable) {
        initIoTable();
      } else {
        ioTable.redraw(true);
      }
    });
  }

  function hideIOSection() {
    if (ioSection) ioSection.style.display = "none";
    container.style.display = "";
  }

  function loadSection(section) {

    // Clean up previous SER resources when switching away
    if (section !== "ser") {
      teardownSer();
      hideSerSection();
    }

    // Clean up previous Relay Word resources when switching away
    if (section !== "relayword") {
      teardownRelayWord();
      hideRelayWordSection();
    }

    // Clean up previous IO resources when switching away
    if (section !== "io") {
      teardownIO();
      hideIOSection();
    }

    switch (section) {

      case "settings":
        container.innerHTML = `
          <h2>⚙ Device Configuration</h2>
          <p>Edit relay configuration parameters here.</p>
        `;
        break;

      case "comtrade":
        container.innerHTML = `
          <h2>📁 COMTRADE Files</h2>
          <p>Relay file name, command, and retrieval functions appear here.</p>
        `;
        break;

      case "event":
        container.innerHTML = `
          <h2>📄 Event Report</h2>
          <p>Detailed event logs and formatted reports appear here.</p>
        `;
        break;

      case "io":
        showIOSection();
        break;

      case "relayword":
        showRelayWordSection();
        break;

      case "ser":
        showSerSection();
        break;
    }
  }

  tabs.forEach(tab => {
    tab.addEventListener("click", function () {
      tabs.forEach(t => t.classList.remove("active"));
      this.classList.add("active");
      loadSection(this.getAttribute("data-section"));
    });
  });

  // Default load — SER tab is active
  loadSection("ser");

});
