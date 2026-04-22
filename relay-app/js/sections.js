/**
 * ============================================================
 *  sections.js â€” Section Controller for Relay Detail Page
 * ============================================================
 *
 *  Handles dynamic section loading from the right sidebar tabs.
 *  The SER section includes full WebSocket connectivity with
 *  ASN.1 BER/TLV decoding, Tabulator table, connection status,
 *  auto-refresh polling, and CSV export â€” matching the proven
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

/**
 * Prefix a command with the current relay ID for server-side routing.
 * e.g. "TAR 0" â†’ "3:TAR 0"
 */
function _prefixCmd(cmd) {
  const relay = getCurrentRelay();
  return relay ? (relay.id + ":" + cmd) : cmd;
}

// ============================================================
//  SER Module State
// ============================================================
let serTable           = null;   // Tabulator instance
let serWorker          = null;   // SER Web Worker
let serRing            = null;   // SharedArrayBuffer ring buffer handle
let serReaderId        = -1;     // Ring buffer reader slot
let serReaderRunning   = false;  // Ring reader loop active
let serAutoRefresh     = null;   // Polling interval handle
let serLastMessageAt   = 0;      // Timestamp of last WS message
let serDataReceived    = false;  // Whether any data has arrived
let serWorkerConnected = false;  // Worker WS connected flag
let serKnownSnos       = new Set(); // tracks sno values already in serTable
let serDirectWs        = null;   // Direct WebSocket (fallback when SharedArrayBuffer unavailable)
let serUsingDirect     = false;  // true when using direct WS fallback

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
 *   0x61 (APPLICATION 1, constructed) â†’ contains 0x30 SEQUENCE records
 *     0x80 record_id  (string)
 *     0x81 timestamp  (string)
 *     0x82 status     (string)
 *     0x83 description(string)
 *     0x84 relay_id   (string)
 *     0x85 relay_name (string)
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
      if (fTlv.tag === 0x84) fields.relayId     = val;
      if (fTlv.tag === 0x85) fields.relayName   = val;
      rOff = fTlv.nextOffset;
    }

    const ts = fields.timestamp || "";
    let date = ts || "-";
    let time = "-";
    const sp = ts.indexOf(" ");
    if (sp !== -1) { date = ts.slice(0, sp); time = ts.slice(sp + 1); }

    const snoVal = Number.parseInt(fields.recordId, 10);
    records.push({
      sno:       Number.isNaN(snoVal) ? (fields.recordId || "-") : snoVal,
      date:      date,
      time:      time,
      element:   fields.description || "-",
      state:     fields.status || "",
      relayId:   fields.relayId   || "",
      relayName: fields.relayName || ""
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
    connecting:   { text: "ðŸŸ¡ Connectingâ€¦",  color: "#d97706" },
    connected:    { text: "ðŸŸ¢ Connected",     color: "#16a34a" },
    synced:       { text: "ðŸŸ¢ Data Synced",   color: "#16a34a" },
    disconnected: { text: "ðŸ”´ Disconnected",  color: "#dc2626" },
    error:        { text: "ðŸ”´ Error",         color: "#dc2626" }
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
    connected:    { t: "Connected â€” waiting for data",     h: "No SER records received from the relay yet." },
    synced:       { t: "Connected â€” waiting for data",     h: "No SER records received from the relay yet." },
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
  if (!v) return `<span class="ser-status--empty">â€”</span>`;
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
  serKnownSnos.clear();
  data.forEach(r => serKnownSnos.add(r.sno));
  serTable = new Tabulator("#ser-table", {
    data: data,
    index: "sno",
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
  if (!serTable) {
    initSerTable(data);
  } else {
    // Only append rows not already in the table â€” no full re-render
    const newRows = data.filter(r => !serKnownSnos.has(r.sno));
    if (newRows.length > 0) {
      serTable.blockRedraw();
      serTable.addData(newRows, false);   // false = append at bottom
      newRows.forEach(r => serKnownSnos.add(r.sno));
      serTable.restoreRedraw();           // single flush, no flicker
    }
  }
  serDataReceived = true;
  updateSerStats();

  // Hide empty state once we have data
  const emptyEl = document.getElementById("ser-live-empty");
  if (emptyEl) emptyEl.classList.add("is-hidden");
}

// ============================================================
//  Worker + Ring Buffer Connection (SER)
// ============================================================

function connectSerWebSocket() {
  _serStopReading();
  stopSerAutoRefresh();
  updateSerConnectionStatus("connecting");

  // Fallback: direct WebSocket from main thread when SharedArrayBuffer
  // is unavailable (page not served with COOP/COEP headers).
  if (typeof SharedArrayBuffer === 'undefined') {
    console.warn('[SER] SharedArrayBuffer not available â€” using direct WebSocket fallback.');
    _connectSerDirect();
    return;
  }

  // Create ring + worker once, reuse on reconnect
  if (!serWorker) {
    serRing = createRingBuffer(512 * 1024);  // 512 KB ring
    serWorker = new Worker('js/ser_worker.js');

    serWorker.postMessage({
      type: 'init',
      buffer: serRing.buffer,
      capacity: serRing.capacity
    });

    serWorker.onmessage = (event) => {
      const msg = event.data;
      if (!msg || !msg.type) return;

      if (msg.type === 'ws_status') {
        serWorkerConnected = msg.status === 'connected';

        if (msg.status === 'connected') {
          updateSerConnectionStatus('connected');
          // Start relay pipeline on the C++ backend (on-demand)
          const relay = getCurrentRelay();
          if (relay) {
            serWorker.postMessage({
              type: 'send',
              payload: JSON.stringify({ action: "start_relay", relay_id: String(relay.id) })
            });
            console.log('[SER] Sent start_relay for relay', relay.id);
          }
          // Explicitly request fresh SER data in case the initial
          // binary push from on_accept() was missed (connection hiccup).
          serWorker.postMessage({ type: 'send', payload: 'getData' });
          serLastMessageAt = Date.now();
          startSerAutoRefresh();
          console.log('[SER] Connected via worker');
        } else if (msg.status === 'disconnected') {
          console.log('[SER] Disconnected');
          updateSerConnectionStatus(serDataReceived ? 'synced' : 'disconnected');
          stopSerAutoRefresh();
        } else if (msg.status === 'error') {
          console.error('[SER] Worker WS error');
          updateSerConnectionStatus('error');
        }
      } else if (msg.type === 'ws_text') {
        // Text messages from server (TAR push, JSON fallback)
        const data = msg.data;

        // TAR batch push (server broadcasts to all connections)
        if (typeof data === 'string' && data.startsWith('TAR_BATCH_ALL:')) {
          _handleTarBatchPush(data);
          return;
        }

        // JSON fallback
        try {
          const json = JSON.parse(data);
          if (Array.isArray(json)) {
            updateSerTable(json);
            serLastMessageAt = Date.now();
          }
        } catch (_) { /* non-JSON text */ }
      } else if (msg.type === 'error') {
        console.error('[SER] Worker error:', msg.message);
      }
    };

    serWorker.onerror = (event) => {
      console.error('[SER] Worker error:', event.message);
      updateSerConnectionStatus('error');
    };
  }

  // Start ring reader
  _serStartRingReader();

  // Connect via worker
  const url = buildSerWsUrl();
  console.log('[SER] Connecting via worker to', url);
  serWorker.postMessage({ type: 'connect', wsUrl: url });
}

// ============================================================
//  Direct WebSocket Fallback (no SharedArrayBuffer / Worker)
// ============================================================

let _serDirectReconnectTimer = null;
let _serDirectReconnectDelay = 3000;
const _SER_RECONNECT_MIN = 3000;
const _SER_RECONNECT_MAX = 30000;

function _connectSerDirect() {
  serUsingDirect = true;
  if (serDirectWs) {
    try { serDirectWs.close(); } catch (_) {}
    serDirectWs = null;
  }

  const url = buildSerWsUrl();
  console.log('[SER-Direct] Connecting to', url);
  const ws = new WebSocket(url);
  ws.binaryType = 'arraybuffer';
  serDirectWs = ws;

  ws.onopen = () => {
    serWorkerConnected = true;
    _serDirectReconnectDelay = _SER_RECONNECT_MIN;  // Reset backoff on success
    updateSerConnectionStatus('connected');

    // Start relay pipeline on the C++ backend
    const relay = getCurrentRelay();
    if (relay) {
      ws.send(JSON.stringify({ action: "start_relay", relay_id: String(relay.id) }));
      console.log('[SER-Direct] Sent start_relay for relay', relay.id);
    }
    // Request fresh SER data
    ws.send('getData');
    serLastMessageAt = Date.now();
    startSerAutoRefresh();
    console.log('[SER-Direct] Connected');
  };

  ws.onmessage = (event) => {
    const data = event.data;

    // Binary â†’ TLV-encoded SER records
    if (data instanceof ArrayBuffer) {
      try {
        let records = decodeSerRecordsFromTlv(data);
        const relay = getCurrentRelay();
        if (relay) {
          records = records.filter(r => r.relayId === String(relay.id));
        }
        if (records.length > 0) {
          updateSerTable(records);
          serLastMessageAt = Date.now();
        }
      } catch (err) {
        console.error('[SER-Direct] TLV decode error:', err);
      }
      return;
    }

    // Text â€” TAR batch push
    if (typeof data === 'string' && data.startsWith('TAR_BATCH_ALL:')) {
      _handleTarBatchPush(data);
      return;
    }

    // Text â€” JSON array fallback
    if (typeof data === 'string') {
      try {
        const json = JSON.parse(data);
        if (Array.isArray(json)) {
          updateSerTable(json);
          serLastMessageAt = Date.now();
        }
      } catch (_) { /* non-JSON text â€” ignore */ }
    }
  };

  ws.onclose = () => {
    serWorkerConnected = false;
    serDirectWs = null;
    console.log('[SER-Direct] Closed');
    updateSerConnectionStatus(serDataReceived ? 'synced' : 'disconnected');
    stopSerAutoRefresh();
    // Auto-reconnect with exponential backoff
    _serDirectReconnectTimer = setTimeout(() => {
      if (serUsingDirect && !serDirectWs) {
        _connectSerDirect();
      }
    }, _serDirectReconnectDelay);
    _serDirectReconnectDelay = Math.min(_serDirectReconnectDelay * 1.5, _SER_RECONNECT_MAX);
  };

  ws.onerror = () => {
    console.error('[SER-Direct] Connection error');
    updateSerConnectionStatus('error');
  };
}

function _serStartRingReader() {
  if (serReaderRunning || !serRing) return;

  serReaderId = serRing.registerReader();
  if (serReaderId < 0) {
    console.error('[SER] All ring reader slots full');
    return;
  }

  serReaderRunning = true;
  let lastSignal = Atomics.load(serRing.header, 1);

  const pump = async () => {
    while (serReaderRunning) {
      // Wait for signal from writer
      if (typeof Atomics.waitAsync === 'function') {
        const result = Atomics.waitAsync(serRing.header, 1, lastSignal);
        if (result && result.value && typeof result.value.then === 'function') {
          await result.value;
        } else {
          await new Promise(r => setTimeout(r, 50));
        }
      } else {
        await new Promise(r => setTimeout(r, 50));
      }

      const current = Atomics.load(serRing.header, 1);
      if (current === lastSignal) continue;
      lastSignal = current;

      // Drain all available payloads
      let payload = serRing.readPayload(serReaderId);
      while (payload) {
        try {
          const buf = payload.buffer.slice(payload.byteOffset, payload.byteOffset + payload.byteLength);
          let records = decodeSerRecordsFromTlv(buf);

          // Filter to current relay (shared WS sends all relays' records)
          const relay = getCurrentRelay();
          if (relay) {
            records = records.filter(r => r.relayId === String(relay.id));
          }

          if (records.length > 0) {
            updateSerTable(records);
            serLastMessageAt = Date.now();
          }
        } catch (err) {
          console.error('[SER] Ring decode error:', err);
        }
        payload = serRing.readPayload(serReaderId);
      }
    }
  };

  pump();
}

function _serStopReading() {
  serReaderRunning = false;
  if (serRing && serReaderId >= 0) {
    serRing.unregisterReader(serReaderId);
    serReaderId = -1;
  }
}

function disconnectSerWebSocket() {
  _serStopReading();
  stopSerAutoRefresh();
  if (serDirectWs) {
    serUsingDirect = false;
    if (_serDirectReconnectTimer) { clearTimeout(_serDirectReconnectTimer); _serDirectReconnectTimer = null; }
    try { serDirectWs.close(); } catch (_) {}
    serDirectWs = null;
  }
  if (serWorker) {
    serWorker.postMessage({ type: 'disconnect' });
  }
  serWorkerConnected = false;
  updateSerConnectionStatus('disconnected');
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
    if (serUsingDirect && serDirectWs && serDirectWs.readyState === WebSocket.OPEN) {
      serDirectWs.send('getData');
    } else if (serWorker && serWorkerConnected) {
      serWorker.postMessage({ type: 'send', payload: 'getData' });
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
  if (serUsingDirect && serDirectWs && serDirectWs.readyState === WebSocket.OPEN) {
    serDirectWs.send('refresh');
  } else if (serWorker && serWorkerConnected) {
    serWorker.postMessage({ type: 'send', payload: 'refresh' });
  } else {
    connectSerWebSocket();
  }
}

function serExportCSV() {
  if (serTable) serTable.download("csv", "ser_records.csv");
}

function serSendQuit() {
  if (serWorker && serWorkerConnected) {
    // Stop the relay pipeline on the C++ backend
    const relay = getCurrentRelay();
    if (relay) {
      serWorker.postMessage({
        type: 'send',
        payload: JSON.stringify({ action: "stop_relay", relay_id: String(relay.id) })
      });
      console.log('[SER] Sent stop_relay for relay', relay.id);
    }
    serWorker.postMessage({ type: 'send', payload: 'QUIT' });
  }
  disconnectSerWebSocket();
}

// ============================================================
//  RELAY WORD (TAR) Module
// ============================================================
//
//  Clean async/await implementation.
//  Sends TAR 0 â†’ TAR 77 sequentially, waits for each response
//  before sending the next, and updates the Tabulator table
//  row-by-row.
//
//  Public API (called from onclick in relay.html):
//    rwFetchAll()   â€” start sequential fetch
//    rwExportCSV()  â€” download CSV
//    rwDisconnect() â€” abort & disconnect
// ============================================================

// â”€â”€ State â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

let rwTable     = null;   // Tabulator instance (created once)
let rwTableReady = Promise.resolve(); // resolves when tableBuilt fires
let rwWs        = null;   // WebSocket connection
let rwAbort     = false;  // Abort signal for in-flight fetch

// â”€â”€ TAR data cache (shared by Relay Word and I/O) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// Per-relay TAR cache map: { "<relayId>": [...parsedRows] }.
// Each relay's TAR data is isolated under its own key so a broadcast
// for relay "1" never overwrites cached rows for relay "2".
let _tarCacheMap = {};
let _tarFetchPromise = null; // In-flight fetch promise (to avoid duplicate fetches)

/** Get cached TAR rows for the current relay (or a specific one). */
function _getCurrentRelayId() {
  const r = (typeof getCurrentRelay === "function") ? getCurrentRelay() : null;
  return r ? String(r.id) : null;
}
function _getTarCache(relayId) {
  const id = relayId || _getCurrentRelayId();
  if (!id) return null;
  return _tarCacheMap[id] || null;
}
function _setTarCache(relayId, rows) {
  if (!relayId) return;
  _tarCacheMap[relayId] = rows;
}
function _clearTarCache(relayId) {
  const id = relayId || _getCurrentRelayId();
  if (!id) return;
  delete _tarCacheMap[id];
}

/**
 * Parse a TAR_BATCH_ALL:<relayId>:[...] message into { relayId, jsonStr }.
 * Returns null if the message is malformed.
 */
function _parseTarBatchMessage(msg) {
  const prefix = "TAR_BATCH_ALL:";
  if (typeof msg !== "string" || !msg.startsWith(prefix)) return null;
  const rest = msg.substring(prefix.length);
  const colonIdx = rest.indexOf(":");
  if (colonIdx < 0) return null;
  return {
    relayId: rest.substring(0, colonIdx),
    jsonStr: rest.substring(colonIdx + 1)
  };
}

/**
 * Handle a server-pushed TAR_BATCH_ALL message (from any WebSocket).
 * Parses the batch, caches it, and renders the Relay Word table if visible.
 */
/**
 * Parse a TAR batch JSON array into row objects for the Tabulator table.
 * @param {Array} entries  Array of {idx, data} objects
 * @returns {Array} Parsed row objects
 */
function _parseTarEntries(entries) {
  const rows = [];
  for (const entry of entries) {
    const parsed = parseTarResponse(entry.data, entry.idx);
    if (parsed) {
      rows.push({
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
      });
    }
  }
  return rows;
}

function _handleTarBatchPush(msg) {
  const parsed = _parseTarBatchMessage(msg);
  if (!parsed) {
    console.warn("[TAR-PUSH] Malformed TAR_BATCH_ALL message â€” ignored");
    return;
  }
  const { relayId, jsonStr } = parsed;
  try {
    const entries = JSON.parse(jsonStr);
    const allRows = _parseTarEntries(entries);
    _setTarCache(relayId, allRows);
    console.log("[TAR-PUSH] Cached", allRows.length, "rows for relay", relayId);

    // Only render/refresh UI if this push is for the relay currently shown.
    const currentId = _getCurrentRelayId();
    if (relayId !== currentId) {
      console.log("[TAR-PUSH] Push is for relay", relayId, "but current page is", currentId, "â€” cached only");
      return;
    }

    _rwSetRowCount(allRows.length);
    // If Relay Word section is visible, render immediately even if table
    // was not initialized yet.
    const rwSec = document.getElementById("relayword-section");
    if (rwSec && rwSec.style.display !== "none") {
      _rwPopulateFromCache();
    }

    // Also refresh I/O from the completed TAR cache.
    _ioSyncFromTarCache();
  } catch (e) {
    console.error("[TAR-PUSH] Failed to parse pushed TAR batch:", e);
  }
}

// â”€â”€ Batch state for FETCH_ALL_TAR â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

let _rwBatchResolve = null;   // resolve() for batch response
let _rwBatchReject  = null;   // reject()  for batch response

// â”€â”€ UI Helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

function _rwBuildWsUrl() {
  const h = (document.getElementById("rw-ws-host") || {}).value?.trim() || "localhost";
  const p = (document.getElementById("rw-ws-port") || {}).value?.trim() || "8765";
  return `ws://${h}:${p}`;
}

function _rwSetStatus(status) {
  const el = document.getElementById("rw-conn-status");
  if (!el) return;
  const map = {
    idle:         { text: "ðŸŸ¡ Idle",           color: "#d97706" },
    connecting:   { text: "ðŸŸ¡ Connectingâ€¦",    color: "#d97706" },
    fetching:     { text: "ðŸ”µ Fetchingâ€¦",      color: "#2563eb" },
    done:         { text: "ðŸŸ¢ Done",           color: "#16a34a" },
    error:        { text: "ðŸ”´ Error",          color: "#dc2626" },
    disconnected: { text: "ðŸ”´ Disconnected",   color: "#dc2626" },
    aborted:      { text: "ðŸŸ  Aborted",        color: "#ea580c" }
  };
  const info = map[status] || map.idle;
  el.textContent = info.text;
  el.style.color = info.color;
}

function _rwSetProgress(current, _total) {
  const container = document.getElementById("rw-spinner-container");
  if (!container) return;

  if (current < 0) {                       // hide
    container.style.display = "none";
    return;
  }
  container.style.display = "";
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

// â”€â”€ TAR Response Parser â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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
function parseTarResponse(text, fallbackIdx) {
  const lines = text.split(/\r?\n/);

  // â”€â”€ Extract targetRow from "TAR n" â”€â”€
  let targetRow = null;
  let dnpIndex  = "â€”";

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

  // Some relays (e.g. SEL-451) omit the "TAR n" / "ROW n" header on
  // per-row responses â€” fall back to the server-provided idx.
  if (targetRow === null && typeof fallbackIdx === "number")
    targetRow = fallbackIdx;

  if (targetRow === null) return null;

  // â”€â”€ Find the values line: exactly 8 tokens, all '0' or '1' â”€â”€
  let labels = Array(8).fill("");
  let values = Array(8).fill(0);
  let foundValues = false;

  for (let i = 0; i < lines.length; i++) {
    const tok = lines[i].trim().split(/\s+/).filter(Boolean);
    if (tok.length !== 8 || !tok.every(t => t === "0" || t === "1")) continue;

    values = tok.map(Number);
    foundValues = true;

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

  if (!foundValues) return null;

  return { targetRow, dnpIndex, labels, values };
}

// â”€â”€ Tabulator â€” Init (once) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

function _rwBitFormatter(cell) {
  const v = cell.getValue();
  if (v === null || v === undefined)
    return '<span style="color:#9ca3af">â€”</span>';

  const label = v.label || "";
  const val   = v.value;
  const on    = val === 1;
  const bg    = on ? "background:#dcfce7;border-radius:4px;" : "";
  const clr   = on ? "#16a34a" : "#9ca3af";

  return `<div style="text-align:center;padding:2px 0;${bg}">
    <div style="font-size:.7em;color:#6b7280;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:90px" title="${label}">${label || "â€”"}</div>
    <div style="font-weight:700;color:${clr};font-size:1.1em">${val ?? "â€”"}</div>
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
    data: [],                          // start empty â€” rows added dynamically
    index: "targetRow",
    layout: "fitColumns",
    height: "600px",
    pagination: false,
    movableColumns: true,
    placeholder: "No Relay Word Data â€” click Fetch All TAR",
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

  // Expose a promise that resolves once Tabulator is fully built
  rwTableReady = new Promise(resolve => {
    rwTable.on("tableBuilt", resolve);
  });
}

function _rwDnpRange(row) {
  const start = row * 8;
  const end = start + 7;
  return `${start}-${end}`;
}

// â”€â”€ WebSocket â€” Connect (returns Promise) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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
    rwWs.binaryType = "arraybuffer";  // avoid Blob handling issues

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
      if (_rwBatchReject) {
        _rwBatchReject(new Error("WebSocket closed unexpectedly"));
        _rwBatchResolve = null;
        _rwBatchReject  = null;
      }
    };

    // Route incoming text messages â€” batch TAR or single-response
    rwWs.onmessage = (event) => {
      const msg = event.data;

      // Skip binary messages (initial SER data / broadcasts)
      if (msg instanceof ArrayBuffer || msg instanceof Blob) {
        console.log("[RW] Received binary (", msg.byteLength || msg.size, "bytes) â€” skipping");
        return;
      }

      if (typeof msg !== "string") {
        console.log("[RW] Received non-string message type:", typeof msg);
        return;
      }

      console.log("[RW] Received WS message (", msg.length, "chars), starts with:", msg.substring(0, 60));

      // â”€â”€ Batch: TAR_BATCH_ALL:<relayId>:[...] â”€â”€
      if (msg.startsWith("TAR_BATCH_ALL:")) {
        const parsed = _parseTarBatchMessage(msg);
        if (!parsed) {
          console.warn("[RW] Malformed TAR_BATCH_ALL â€” ignored");
          return;
        }
        console.log("[RW] TAR_BATCH_ALL received for relay", parsed.relayId, "(", parsed.jsonStr.length, "chars)");
        if (_rwBatchResolve) {
          const res = _rwBatchResolve;
          _rwBatchResolve = null;
          _rwBatchReject  = null;
          res(parsed);   // resolve with { relayId, jsonStr }
        } else {
          console.warn("[RW] TAR_BATCH_ALL arrived but no pending resolve â€” handling as push");
          _handleTarBatchPush(msg);
        }
        return;
      }



    };
  });
}

// â”€â”€ Core â€” fetchAllTAR()  (streaming batch) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

/**
 * Sends a single FETCH_ALL_TAR command to the server. The server
 * collects all TAR rows internally, then sends the entire batch
 * back as a single TAR_BATCH_ALL JSON message. The UI displays
 * a loading spinner until all data arrives, then renders all rows.
 * Results are cached so subsequent calls (and I/O) are instant.
 */
async function fetchAllTAR() {
  const currentRelayId = _getCurrentRelayId();

  // If cache exists for the current relay, just populate the table
  if (_getTarCache(currentRelayId)) {
    _rwPopulateFromCache();
    return;
  }

  // If a fetch is already in flight, wait for it
  if (_tarFetchPromise) {
    await _tarFetchPromise;
    _rwPopulateFromCache();
    return;
  }

  // Prevent double invocation
  _rwSetFetchBtn(true);
  rwAbort = false;

  _tarFetchPromise = (async () => {
    try {
      // 1. Connect
      const ws = await _rwEnsureConnection();
      _rwSetStatus("fetching");
      _rwSetProgress(0, 1);    // show spinner
      _rwSetRowCount(0);

      // 1b. Ensure relay pipeline is running before fetching TAR data.
      //     The server processes messages sequentially per connection,
      //     so start_relay will complete before FETCH_ALL_TAR is read.
      const relay = getCurrentRelay();
      if (relay) {
        ws.send(JSON.stringify({ action: "start_relay", relay_id: String(relay.id) }));
      }

      // 2. Send batch command & wait for TAR_BATCH_ALL response
      const batchResult = await new Promise((resolve, reject) => {
        _rwBatchResolve = resolve;
        _rwBatchReject  = reject;

        // Timeout: 10 min â€” matches the backend wait_for duration.
        // Slow relays (~78 TAR rows Ã— 2s each) can take 150+ seconds,
        // so 2 min was too short and caused false timeout errors.
        const timer = setTimeout(() => {
          _rwBatchResolve = null;
          _rwBatchReject  = null;
          reject(new Error("Timeout waiting for FETCH_ALL_TAR to complete"));
        }, 600_000);

        const origResolve = resolve;
        _rwBatchResolve = (data) => { clearTimeout(timer); origResolve(data); };
        _rwBatchReject  = (err)  => { clearTimeout(timer); reject(err); };

        console.log("[RW] Sending FETCH_ALL_TAR (batch)");
        ws.send(_prefixCmd("FETCH_ALL_TAR"));
      });

      // batchResult = { relayId, jsonStr }
      const batchRelayId = batchResult.relayId;
      const jsonStr      = batchResult.jsonStr;

      // 3. Parse all rows and cache them under the responding relay's id
      if (!rwAbort) {
        const entries = JSON.parse(jsonStr);
        const allRows = [];

        for (const entry of entries) {
          const parsed = parseTarResponse(entry.data, entry.idx);
          if (!parsed) {
            console.warn(`[RW] Failed to parse TAR ${entry.idx}`);
            continue;
          }
          allRows.push({
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
          });
        }

        _setTarCache(batchRelayId, allRows);

        // Only render if the response is for the relay currently displayed
        if (batchRelayId === _getCurrentRelayId()) {
          _rwPopulateFromCache();
        } else {
          console.log("[RW] FETCH_ALL_TAR response was for relay", batchRelayId,
                      "but current page is", _getCurrentRelayId(), "â€” cached only");
        }

        // If relay returned 0 parseable rows, TAR is likely unsupported
        if (allRows.length === 0) {
          _rwSetStatus("done");
          _rwSetProgress(-1);
          console.warn(`[RW] FETCH_ALL_TAR returned 0 parseable rows â€” relay may not support TAR`);
          if (typeof showToast === "function") {
            showToast("This relay does not appear to support TAR commands", "warning");
          }
        } else {
          console.log(`[RW] FETCH_ALL_TAR complete â€” ${entries.length} rows received, ${allRows.length} parsed & cached`);
        }
      }

    } catch (err) {
      _rwSetProgress(-1);
      if (rwAbort) {
        // User navigated away â€” leave the per-relay cache empty so the next
        // visit to Relay Word retries instead of showing an empty table.
        console.log("[RW] fetchAllTAR cancelled by navigation");
        _rwSetStatus("idle");
      } else {
        console.error("[RW] fetchAllTAR error:", err);
        _rwSetStatus("error");
        // Do NOT cache empty here â€” a timeout or network error just means
        // the relay was slow or unreachable, not that it lacks TAR support.
        // Leaving the per-relay cache empty lets the next visit retry.
        // (The "relay doesn't support TAR" case is handled in the success
        //  path above where allRows.length === 0 already caches an empty array.)
        if (typeof showToast === "function") {
          showToast(`TAR fetch failed: ${err.message}`, "error");
        }
      }
    } finally {
      _rwBatchResolve = null;
      _rwBatchReject  = null;
      _rwSetFetchBtn(false);
      _tarFetchPromise = null;
    }
  })();

  await _tarFetchPromise;
}

/** Populate the Relay Word table from cached data for the current relay. */
async function _rwPopulateFromCache() {
  const rows = _getTarCache();
  if (!rows) return;
  if (!rwTable) initRwTable();
  await rwTableReady;
  rwTable.blockRedraw();
  rwTable.setData(rows);
  rwTable.restoreRedraw();
  _rwSetRowCount(rows.length);
  _rwSetProgress(-1);
  _rwSetStatus("done");
  _rwSetFetchBtn(false);
}

// â”€â”€ Disconnect helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

function _rwDisconnectRaw() {
  if (rwWs) {
    rwWs.onopen    = null;
    rwWs.onclose   = null;
    rwWs.onerror   = null;
    rwWs.onmessage = null;
    try { rwWs.close(); } catch (_) {}
    rwWs = null;
  }
  _rwBatchResolve = null;
  _rwBatchReject  = null;
}

// â”€â”€ Public Actions (called from onclick in relay.html) â”€â”€â”€â”€â”€â”€

function rwFetchAll() {
  _clearTarCache();        // force re-fetch for the CURRENT relay only
  fetchAllTAR();           // fire-and-forget (async)
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
//  Fetches all TAR rows (0â€“77) â€” same as Relay Word â€” but only
//  keeps rows where at least one bit label starts with "IN" or
//  "OUT".  Shows them in the EXACT same table format as Relay
//  Word (Target Row | DNP Index | bit7â€¦bit0) with the same
//  label+value cell renderer.
//
//  Public API (called from onclick in relay.html):
//    ioFetchAll()    â€” start fetch, filter, display
//    ioExportCSV()   â€” download CSV
//    ioDisconnect()  â€” abort & disconnect
//    ioSetFilter(f)  â€” filter by 'all' | 'in' | 'out'
// ============================================================

// â”€â”€ State â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

let ioTable      = null;   // Tabulator instance
let ioAbort      = false;  // Abort signal
let _ioAllRows   = [];     // All collected I/O rows (RW format, unfiltered)
let _ioFilter    = "all";  // Current filter: 'all' | 'in' | 'out'

function _ioSetStatus(status) {
  const el = document.getElementById("io-conn-status");
  if (!el) return;
  const map = {
    idle:         { text: "ðŸŸ¡ Idle",           color: "#d97706" },
    connecting:   { text: "ðŸŸ¡ Connectingâ€¦",    color: "#d97706" },
    fetching:     { text: "ðŸ”µ Fetchingâ€¦",      color: "#2563eb" },
    done:         { text: "ðŸŸ¢ Done",           color: "#16a34a" },
    error:        { text: "ðŸ”´ Error",          color: "#dc2626" },
    disconnected: { text: "ðŸ”´ Disconnected",   color: "#dc2626" },
    aborted:      { text: "ðŸŸ  Aborted",        color: "#ea580c" }
  };
  const info = map[status] || map.idle;
  el.textContent = info.text;
  el.style.color = info.color;
}

function _ioSetProgress(current, _total) {
  const container = document.getElementById("io-spinner-container");
  if (!container) return;

  if (current < 0) {
    container.style.display = "none";
    return;
  }
  container.style.display = "";
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

// â”€â”€ Tabulator â€” Init (same format as Relay Word table) â”€â”€â”€â”€â”€â”€

function initIoTable() {
  if (ioTable) return;

  const bitCol = (title, field) => ({
    title, field,
    hozAlign: "center",
    formatter: _rwBitFormatter,     // reuse the same cell renderer
    headerSort: true,
    width: 100,
    sorter: (a, b) => ((a?.value ?? -1) - (b?.value ?? -1))
  });

  ioTable = new Tabulator("#io-table", {
    data: [],
    index: "targetRow",
    layout: "fitColumns",
    height: "600px",
    pagination: false,
    movableColumns: true,
    placeholder: "No I/O Data â€” click Fetch I/O to scan relay",
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

// â”€â”€ Helper â€” check if a row has any IN/OUT label â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

function _ioRowHasLabel(row, prefix) {
  for (const f of ["bit7","bit6","bit5","bit4","bit3","bit2","bit1","bit0"]) {
    const lbl = (row[f]?.label || "").toUpperCase();
    if (lbl.startsWith(prefix)) return true;
  }
  return false;
}

function _ioRowHasIO(row) {
  return _ioRowHasLabel(row, "IN") || _ioRowHasLabel(row, "OUT");
}

// â”€â”€ Tabulator â€” Filter â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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
  if (_ioFilter === "in")  data = _ioAllRows.filter(r => _ioRowHasLabel(r, "IN"));
  if (_ioFilter === "out") data = _ioAllRows.filter(r => _ioRowHasLabel(r, "OUT"));
  ioTable.setData(data);
  _ioSetRowCount(data.length);
}

/**
 * Rebuild I/O cache/table from the current full TAR cache.
 */
function _ioSyncFromTarCache() {
  const rows = _getTarCache();
  if (!rows) return;

  _ioAllRows = rows.filter(_ioRowHasIO);

  const ioSec = document.getElementById("io-section");
  if (!(ioSec && ioSec.style.display !== "none")) return;

  if (!ioTable) initIoTable();
  _ioApplyFilter();
  _ioSetProgress(-1);
  _ioSetStatus("done");
}

// â”€â”€ Core â€” fetchAllIO()  (uses shared TAR cache) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

/**
 * Uses the shared TAR cache from fetchAllTAR(). If data hasn't
 * been fetched yet, triggers the fetch and waits. Filters rows
 * to only keep those with IN/OUT labels.
 */
async function fetchAllIO() {
  _ioSetFetchBtn(true);
  ioAbort = false;
  _ioAllRows = [];

  try {
    // If cache doesn't exist yet for the current relay, trigger the shared fetch
    let cachedRows = _getTarCache();
    if (!cachedRows) {
      _ioSetStatus("fetching");
      _ioSetProgress(0, 1);    // show spinner
      _ioSetRowCount(0);
      await fetchAllTAR();     // populates per-relay cache
      cachedRows = _getTarCache();
    }

    if (!cachedRows || ioAbort) return;

    // Filter cached rows for I/O labels
    for (const row of cachedRows) {
      if (_ioRowHasIO(row)) {
        _ioAllRows.push(row);
      }
    }

    _ioSetProgress(-1);
    _ioSetStatus("done");
    _ioApplyFilter();
    console.log(`[IO] Filtered from cache â€” ${_ioAllRows.length} I/O rows found`);

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

// â”€â”€ Disconnect helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

function _ioDisconnectRaw() {
  // No-op: I/O module uses shared TAR cache, no own WebSocket
}


// â”€â”€ Public Actions â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

function ioFetchAll() {
  _clearTarCache();   // force re-fetch for the CURRENT relay only
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

// ============================================================
//  COMTRADE (SEL-451 only) Module (auto-cached, no manual fetch)
// ============================================================
//  - Backend broadcasts COMTRADE_DIR:<relayId>:<raw file list> on pipeline start
//  - This module listens for the broadcast, caches per-relay, and renders on section open
//  - No manual fetch or WebSocket command is used
// ============================================================

let ctrWs      = null;

// Per-relay cache: relayId → raw FILE DIR EVENTS response
const ctrFileDirCache = {};

function _ctrSetStatus(status, custom) {
  const el = document.getElementById("ctr-conn-status");
  if (!el) return;
  const map = {
    idle:       { text: "🟡 Idle",        color: "#d97706" },
    done:       { text: "🟢 Ready",       color: "#16a34a" },
    error:      { text: "🔴 Error",       color: "#dc2626" }
  };
  const info = map[status] || map.idle;
  el.textContent = custom || info.text;
  el.style.color = info.color;
}
function _ctrEnsureConnection() {
  return new Promise((resolve, reject) => {
    if (ctrWs && ctrWs.readyState === WebSocket.OPEN) return resolve(ctrWs);
    _ctrDisconnectRaw();
    _ctrSetStatus("connecting");
    const url = _ctrBuildWsUrl();
    console.log("[CTR] Connecting to", url);
    ctrWs = new WebSocket(url);

    ctrWs.onopen  = () => { console.log("[CTR] Connected"); resolve(ctrWs); };
    ctrWs.onerror = (err) => { console.error("[CTR] Error", err); reject(new Error("WebSocket connection failed")); };
    ctrWs.onclose = () => {
      console.log("[CTR] Closed");
      if (_ctrReject) { _ctrReject(new Error("WebSocket closed")); _ctrResolve = null; _ctrReject = null; }
    };
    ctrWs.onmessage = (event) => {
      if (typeof event.data !== "string") return;
      if (_ctrResolve) { const r = _ctrResolve; _ctrResolve = null; _ctrReject = null; r(event.data); }
    };
  });
}

async function _ctrSendCommand(cmd) {
  const ws = await _ctrEnsureConnection();
  if (ws.readyState !== WebSocket.OPEN) throw new Error("WebSocket not open");
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      _ctrResolve = null; _ctrReject = null;
      reject(new Error("Timeout"));
    }, 60_000);
    _ctrResolve = (d) => { clearTimeout(timer); resolve(d); };
    _ctrReject  = (e) => { clearTimeout(timer); reject(e); };
    console.log("[CTR] Sending:", cmd);
    ws.send(_prefixCmd(cmd));
  });
}

function _ctrDisconnectRaw() {
  if (ctrWs) {
    ctrWs.onopen = null; ctrWs.onclose = null; ctrWs.onerror = null; ctrWs.onmessage = null;
    try { ctrWs.close(); } catch (_) {}
    ctrWs = null;
  }
  _ctrResolve = null; _ctrReject = null;
}

function _ctrEscHtml(s) {
  const d = document.createElement("div");
  d.textContent = s == null ? "" : String(s);
  return d.innerHTML;
}

function _ctrDownloadText(filename, text) {
  const blob = new Blob([text], { type: "application/octet-stream" });
  const url  = URL.createObjectURL(blob);
  const a    = document.createElement("a");
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

/**
 * Parse a `FILE DIR EVENTS` response block.
 * Each data line: `<filename>  <size>  <date>  <time>`.
 * Header, border, and prompt lines are skipped.
 *
 * @returns {{name:string, num:number, size:string, date:string, time:string}[]}
 */
function _ctrParseFileDir(response) {
  const out = [];
  const lines = String(response || "").split(/\r?\n/);
  for (let raw of lines) {
    const line = raw.replace(/\s+$/, "");
    if (!line) continue;
    const trimmed = line.trim();
    if (/^=>/.test(trimmed)) continue;               // prompt
    if (/^[-=*\s]+$/.test(trimmed)) continue;         // separator
    const tokens = trimmed.split(/\s+/);
    const name = tokens[0];
    if (!name || name.indexOf(".") < 0) continue;
    const dot = name.lastIndexOf(".");
    const ext = name.substring(dot + 1).toUpperCase();
    if (ext !== "CFG" && ext !== "DAT" && ext !== "CEV" && ext !== "HIS" && ext !== "EVE") continue;
    // Event number = trailing digit run before the dot
    let numStart = dot;
    while (numStart > 0 && /\d/.test(name[numStart - 1])) numStart--;
    const numStr = name.substring(numStart, dot);
    if (!numStr) continue;
    const num = parseInt(numStr, 10);
    if (!Number.isFinite(num)) continue;
    out.push({
      name,
      num,
      size: tokens[1] || "",
      date: tokens[2] || "",
      time: tokens[3] || ""
    });
  }
  return out;
}

function _ctrRenderFiles(files) {
  const container = document.getElementById("ctr-file-list");
  if (!container) return;
  if (!files.length) {
    container.innerHTML =
      `<div class="set-empty">No .cfg or .dat files found in FILE DIR EVENTS.</div>`;
    return;
  }
  const parts = files.map((f, i) => {
    const extUp    = f.name.substring(f.name.lastIndexOf(".") + 1).toUpperCase();
    const badgeCls = extUp === "CFG" ? "ctf-file-card__badge--cfg" : "ctf-file-card__badge--dat";
    const safeName = _ctrEscHtml(f.name);
    const meta = _ctrEscHtml(
      [f.size && `${f.size} bytes`, f.date, f.time].filter(Boolean).join(" Â· ")
    );
    return (
      `<div class="ctf-file-card">` +
        `<div class="ctf-file-card__header">` +
          `<div class="ctf-file-card__info">` +
            `<span class="ctf-file-card__icon">ðŸ“„</span>` +
            `<span class="ctf-file-card__name">${safeName}</span>` +
            `<span class="ctf-file-card__badge ${badgeCls}">${extUp}</span>` +
          `</div>` +
          `<div class="ctf-file-card__actions">` +
            `<button class="btn btn--primary btn--sm ctr-dl-btn" ` +
                   `data-ctr-idx="${i}">â¬‡ Download</button>` +
          `</div>` +
        `</div>` +
        (meta ? `<div class="ctf-file-card__meta">${meta}</div>` : "") +
      `</div>`
    );
  });
  container.innerHTML = parts.join("");

  // Wire download buttons (avoid inline handlers with dynamic filenames)
  container.querySelectorAll(".ctr-dl-btn").forEach(btn => {
    btn.addEventListener("click", async () => {
      const idx = parseInt(btn.getAttribute("data-ctr-idx"), 10);
      const file = files[idx];
      if (!file) return;
      const extUp = file.name.substring(file.name.lastIndexOf(".") + 1).toUpperCase();
      const prevText = btn.textContent;
      btn.disabled = true;
      btn.textContent = "Fetchingâ€¦";
      try {
        const cmd = (extUp === "CFG" ? "CTR C " : "CTR D ") + file.num;
        const response = await _ctrSendCommand(cmd);
        _ctrDownloadText(file.name, response);
        btn.textContent = "âœ… Saved";
        setTimeout(() => {
          btn.disabled = false;
          btn.textContent = prevText;
        }, 1500);
      } catch (err) {
        console.error("[CTR] download error:", err);
        if (typeof showToast === "function")
          showToast(`Download failed: ${err.message}`, "error");
        btn.disabled = false;
        btn.textContent = prevText;
      }
    });
  });
}


// Listen for COMTRADE_DIR:<relayId>:<payload> broadcasts
if (window && typeof window.addEventListener === "function") {
  window.addEventListener("message", function(event) {
    if (!event.data || typeof event.data !== "string") return;
    if (!event.data.startsWith("COMTRADE_DIR:")) return;
    const idx1 = event.data.indexOf(":", 14);
    if (idx1 < 0) return;
    const relayId = event.data.substring(14, idx1);
    const payload = event.data.substring(idx1 + 1);
    ctrFileDirCache[relayId] = payload;
    // If this relay is currently shown, re-render
    const relay = getCurrentRelay();
    if (relay && relay.id === relayId) {
      ctrRenderFileDirForCurrentRelay();
    }
  });
}

function ctrRenderFileDirForCurrentRelay() {
  const relay = getCurrentRelay();
  const list = document.getElementById("ctr-file-list");
  if (!relay || !list) return;
  const raw = ctrFileDirCache[relay.id];
  if (!raw) {
    list.innerHTML = `<div class="set-empty">No COMTRADE file list available yet.</div>`;
    _ctrSetStatus("idle");
    return;
  }
  const all = _ctrParseFileDir(raw);
  const filtered = all.filter(f => {
    const ext = f.name.substring(f.name.lastIndexOf(".") + 1).toUpperCase();
    return ext === "CFG" || ext === "DAT";
  });
  filtered.sort((a, b) => {
    if (a.num !== b.num) return b.num - a.num;
    const ea = a.name.substring(a.name.lastIndexOf(".") + 1).toUpperCase();
    const eb = b.name.substring(b.name.lastIndexOf(".") + 1).toUpperCase();
    return ea.localeCompare(eb);
  });
  _ctrRenderFiles(filtered);
  const nCfg = filtered.filter(f => f.name.toUpperCase().endsWith(".CFG")).length;
  const nDat = filtered.filter(f => f.name.toUpperCase().endsWith(".DAT")).length;
  _ctrSetStatus("done", `🟢 ${nCfg} CFG · ${nDat} DAT`);
}

// ============================================================
//  SETTINGS (SHOSET) Module
// ============================================================
//
//  Sends the SHOSET command over WebSocket, parses the
//  section-header + key := value response, and renders it
//  as grouped cards inside #set-content.
//
//  Public API:
//    setFetchSettings()  â€” fetch via SHOSET
//    setClearSettings()  â€” clear and disconnect
// ============================================================

let setWs       = null;
let _setResolve  = null;
let _setReject   = null;

function _setBuildWsUrl() {
  const h = (document.getElementById("ser-ws-host") || {}).value?.trim() || "localhost";
  const p = (document.getElementById("ser-ws-port") || {}).value?.trim() || "8765";
  return `ws://${h}:${p}`;
}

function _setSetStatus(status) {
  const el = document.getElementById("set-conn-status");
  if (!el) return;
  const map = {
    idle:       { text: "ðŸŸ¡ Idle",        color: "#d97706" },
    connecting: { text: "ðŸŸ¡ Connectingâ€¦", color: "#d97706" },
    fetching:   { text: "ðŸ”µ Fetchingâ€¦",   color: "#2563eb" },
    done:       { text: "ðŸŸ¢ Done",        color: "#16a34a" },
    error:      { text: "ðŸ”´ Error",       color: "#dc2626" }
  };
  const info = map[status] || map.idle;
  el.textContent = info.text;
  el.style.color = info.color;
}

function _setSetFetchBtn(disabled) {
  const btn = document.getElementById("set-fetch-btn");
  if (!btn) return;
  btn.disabled = disabled;
  btn.style.opacity = disabled ? "0.5" : "1";
  btn.style.pointerEvents = disabled ? "none" : "";
}

function _setEnsureConnection() {
  return new Promise((resolve, reject) => {
    if (setWs && setWs.readyState === WebSocket.OPEN) return resolve(setWs);
    _setDisconnectRaw();
    _setSetStatus("connecting");
    const url = _setBuildWsUrl();
    console.log("[SET] Connecting to", url);
    setWs = new WebSocket(url);

    setWs.onopen = () => { console.log("[SET] Connected"); resolve(setWs); };
    setWs.onerror = (err) => { console.error("[SET] Error", err); reject(new Error("WebSocket connection failed")); };
    setWs.onclose = () => {
      console.log("[SET] Closed");
      if (_setReject) { _setReject(new Error("WebSocket closed")); _setResolve = null; _setReject = null; }
    };
    setWs.onmessage = (event) => {
      if (typeof event.data !== "string") return;
      if (_setResolve) { const r = _setResolve; _setResolve = null; _setReject = null; r(event.data); }
    };
  });
}

async function _setSendCommand(cmd) {
  const ws = await _setEnsureConnection();
  if (ws.readyState !== WebSocket.OPEN) throw new Error("WebSocket not open");
  return new Promise((resolve, reject) => {
    _setResolve = resolve;
    _setReject  = reject;
    const timer = setTimeout(() => { _setResolve = null; _setReject = null; reject(new Error("Timeout")); }, 30_000);
    const origR = resolve, origE = reject;
    _setResolve = (d) => { clearTimeout(timer); origR(d); };
    _setReject  = (e) => { clearTimeout(timer); origE(e); };
    console.log("[SET] Sending:", cmd);
    ws.send(_prefixCmd(cmd));
  });
}

function _setDisconnectRaw() {
  if (setWs) {
    setWs.onopen = null; setWs.onclose = null; setWs.onerror = null; setWs.onmessage = null;
    try { setWs.close(); } catch (_) {}
    setWs = null;
  }
  _setResolve = null; _setReject = null;
}

/**
 * Parse the SHOSET text response into structured groups.
 *
 * Format:
 *   SHOSET
 *   General
 *   Identifier and Scaling
 *   MID      := FEEDER 1
 *   CTR      := 1.0000      PTR      := 1.0000      VOLT_SCA := KILO
 *   ...
 *   Instrument Transformer Compensation
 *   Enable Instrument Transformer Compensation
 *   EITCI    := N           EITCV    := N
 *   Demand Metering
 *
 * Returns: [{ title, subtitle, entries: [{key, value}] }, ...]
 */
function parseShosetResponse(text) {
  const lines          = text.split(/\r?\n/);
  const groups         = [];
  let pendingHeaders   = [];   // headers collected before KV data
  let current          = null; // { title, subtitle, entries }

  function flushGroup() {
    if (current && current.entries.length > 0) groups.push(current);
    current = null;
  }

  function flushHeaders() {
    if (pendingHeaders.length === 0) return;
    flushGroup();
    const title    = pendingHeaders[0];
    const subtitle = pendingHeaders.length > 1 ? pendingHeaders.slice(1).join(" \u2014 ") : null;
    current = { title: title, subtitle: subtitle, entries: [] };
    pendingHeaders = [];
  }

  for (const raw of lines) {
    const line = raw.trim();
    if (!line) continue;
    if (/^\s*=>\s*$/.test(line)) continue;
    if (/^\s*SHOSET\s*$/i.test(line)) continue;
    if (/^Press RETURN to continue$/i.test(line)) continue;

    // Lines containing := are key-value pairs
    if (line.includes(":=")) {
      // Flush any pending headers into a new group
      if (pendingHeaders.length > 0) flushHeaders();
      // If no group exists yet, create a default one
      if (!current) current = { title: "General", subtitle: null, entries: [] };

      // Parse all  KEY := VALUE  patterns in the line.
      const kvRegex = /([A-Za-z0-9_]+)\s*:=\s*(.*?)(?=\s{2,}[A-Za-z0-9_]+\s*:=|$)/g;
      let m;
      while ((m = kvRegex.exec(line)) !== null) {
        current.entries.push({ key: m[1].trim(), value: m[2].trim() });
      }
    } else {
      // Section / sub-section header line
      // If we already have KV entries, close the current group first
      if (current && current.entries.length > 0) {
        flushGroup();
        pendingHeaders = [];
      }
      pendingHeaders.push(line);
    }
  }
  flushGroup();
  return groups;
}

/**
 * Render parsed SHOSET groups into #set-content as styled cards.
 */
function renderShosetSettings(groups) {
  const container = document.getElementById("set-content");
  if (!container) return;

  if (groups.length === 0) {
    container.innerHTML = '<div class="set-empty">No settings found in SHOSET response.</div>';
    return;
  }

  let html = '';
  for (const group of groups) {
    html += '<div class="set-group">';
    html += `<h3 class="set-group__title">${_escHtml(group.title)}</h3>`;
    if (group.subtitle) {
      html += `<div class="set-group__subtitle">${_escHtml(group.subtitle)}</div>`;
    }
    html += '<div class="set-group__grid">';
    for (const entry of group.entries) {
      html += `<div class="set-kv">`;
      html += `<span class="set-kv__key">${_escHtml(entry.key)}</span>`;
      html += `<span class="set-kv__val">${_escHtml(entry.value)}</span>`;
      html += `</div>`;
    }
    html += '</div></div>';
  }
  container.innerHTML = html;
}

function _escHtml(s) {
  const d = document.createElement("div");
  d.textContent = s;
  return d.innerHTML;
}

async function setFetchSettings() {
  _setSetFetchBtn(true);
  try {
    await _setEnsureConnection();
    _setSetStatus("fetching");
    const response = await _setSendCommand("SHOSET");
    const groups   = parseShosetResponse(response);
    renderShosetSettings(groups);
    _setSetStatus("done");
    console.log("[SET] SHOSET fetched â€”", groups.length, "groups");
  } catch (err) {
    console.error("[SET] SHOSET error:", err);
    _setSetStatus("error");
    if (typeof showToast === "function") showToast(`Settings fetch failed: ${err.message}`, "error");
  } finally {
    _setSetFetchBtn(false);
  }
}

function setClearSettings() {
  _setDisconnectRaw();
  const container = document.getElementById("set-content");
  if (container) container.innerHTML = '<div class="set-empty">Loading relay configuration via SHOSET...</div>';
  _setSetStatus("idle");
  _setSetFetchBtn(false);
  console.log("[SET] Cleared");
}

function setDisconnect() {
  _setDisconnectRaw();
  _setSetStatus("idle");
  _setSetFetchBtn(false);
}

// ============================================================
//  EVENT REPORT Module
// ============================================================
//
//  Sends  EVE <n> R  over WebSocket, parses the tabular
//  Currents / Voltages response from the SEL relay, and
//  populates a Tabulator table.
//
//  Public API (called from onclick in relay.html):
//    evFetchReport()   â€” fetch one event report
//    evExportCSV()     â€” download CSV
//    evClearRecords()  â€” clear table and disconnect
// ============================================================

// â”€â”€ State â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

let evTable    = null;   // Tabulator instance
let evWs       = null;   // WebSocket connection
let evAbort    = false;  // Abort signal
let _evAllRows = [];     // Accumulated event rows

// â”€â”€ Promise queue for request-response over WebSocket â”€â”€â”€â”€â”€â”€â”€

let _evResolve = null;
let _evReject  = null;

// â”€â”€ UI Helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

function _evBuildWsUrl() {
  // Reuse the SER host/port inputs so the user doesn't need to enter them twice
  const h = (document.getElementById("ser-ws-host") || {}).value?.trim() || "localhost";
  const p = (document.getElementById("ser-ws-port") || {}).value?.trim() || "8765";
  return `ws://${h}:${p}`;
}

function _evSetStatus(status) {
  const el = document.getElementById("ev-conn-status");
  if (!el) return;
  const map = {
    idle:         { text: "ðŸŸ¡ Idle",           color: "#d97706" },
    connecting:   { text: "ðŸŸ¡ Connectingâ€¦",    color: "#d97706" },
    fetching:     { text: "ðŸ”µ Fetchingâ€¦",      color: "#2563eb" },
    done:         { text: "ðŸŸ¢ Done",           color: "#16a34a" },
    error:        { text: "ðŸ”´ Error",          color: "#dc2626" },
    disconnected: { text: "ðŸ”´ Disconnected",   color: "#dc2626" },
    aborted:      { text: "ðŸŸ  Aborted",        color: "#ea580c" }
  };
  const info = map[status] || map.idle;
  el.textContent = info.text;
  el.style.color = info.color;
}

function _evSetRecordCount(n) {
  const el = document.getElementById("ev-record-count");
  if (el) el.textContent = n;
}

function _evSetFetchBtn(disabled) {
  const btn = document.getElementById("ev-fetch-btn");
  if (!btn) return;
  btn.disabled = disabled;
  btn.style.opacity = disabled ? "0.5" : "1";
  btn.style.pointerEvents = disabled ? "none" : "";
}

// â”€â”€ Tabulator Initialisation â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

function initEvTable() {
  if (evTable) { evTable.redraw(true); return; }
  evTable = new Tabulator("#ev-table", {
    data: [],
    layout: "fitColumns",
    pagination: true,
    paginationSize: 50,
    paginationSizeSelector: [10, 20, 50, 100],
    movableColumns: true,
    placeholder: "No Event Records â€” Enter an event number and click Fetch",
    height: "500px",
    columns: [
      {
        title: "#",
        field: "sample",
        width: 70,
        hozAlign: "center",
        headerSort: true,
        sorter: "number",
        frozen: true
      },
      {
        title: "IA",
        field: "IA",
        width: 150,
        hozAlign: "right",
        headerSort: true,
        headerFilter: "input"
      },
      {
        title: "IB",
        field: "IB",
        width: 150,
        hozAlign: "right",
        headerSort: true,
        headerFilter: "input"
      },
      {
        title: "IC",
        field: "IC",
        width: 150,
        hozAlign: "right",
        headerSort: true,
        headerFilter: "input"
      },
      {
        title: "IN",
        field: "IN",
        width: 150,
        hozAlign: "right",
        headerSort: true,
        headerFilter: "input"
      },
      {
        title: "VAB",
        field: "VAB",
        width: 150,
        hozAlign: "right",
        headerSort: true,
        headerFilter: "input"
      },
      {
        title: "VBC",
        field: "VBC",
        width: 150,
        hozAlign: "right",
        headerSort: true,
        headerFilter: "input"
      },
      {
        title: "VCA",
        field: "VCA",
        width: 150,
        hozAlign: "right",
        headerSort: true,
        headerFilter: "input"
      }
    ],
    initialSort: [{ column: "sample", dir: "asc" }]
  });
}

// â”€â”€ WebSocket â€” Ensure connection â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

function _evEnsureConnection() {
  return new Promise((resolve, reject) => {
    if (evWs && evWs.readyState === WebSocket.OPEN) {
      return resolve(evWs);
    }

    // Clean up any previous socket
    _evDisconnectRaw();

    _evSetStatus("connecting");
    const url = _evBuildWsUrl();
    console.log("[EVE] Connecting to", url);
    evWs = new WebSocket(url);

    evWs.onopen = () => {
      console.log("[EVE] Connected");
      resolve(evWs);
    };

    evWs.onerror = (err) => {
      console.error("[EVE] Connection error:", err);
      reject(new Error("WebSocket connection failed"));
    };

    evWs.onclose = () => {
      console.log("[EVE] Closed");
      if (_evReject) {
        _evReject(new Error("WebSocket closed unexpectedly"));
        _evResolve = null;
        _evReject  = null;
      }
    };

    evWs.onmessage = (event) => {
      if (typeof event.data !== "string") return;
      if (_evResolve) {
        const res = _evResolve;
        _evResolve = null;
        _evReject  = null;
        res(event.data);
      }
    };
  });
}

// â”€â”€ Send one command and wait for response â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

async function _evSendCommand(cmd) {
  const ws = await _evEnsureConnection();

  if (ws.readyState !== WebSocket.OPEN) {
    throw new Error("WebSocket is not open");
  }

  return new Promise((resolve, reject) => {
    _evResolve = resolve;
    _evReject  = reject;

    const timer = setTimeout(() => {
      _evResolve = null;
      _evReject  = null;
      reject(new Error(`Timeout waiting for response to "${cmd}"`));
    }, 30_000);

    const origResolve = resolve;
    const origReject  = reject;
    _evResolve = (data) => { clearTimeout(timer); origResolve(data); };
    _evReject  = (err)  => { clearTimeout(timer); origReject(err); };

    console.log("[EVE] Sending:", cmd);
    ws.send(_prefixCmd(cmd));
  });
}

// â”€â”€ Parse EVE n R response â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

/**
 * Parse the tabular "EVE n R" response from a SEL relay.
 *
 * Expected format:
 *   Currents (Amps Pri)                  Voltages (Volts Pri)
 *   IA      IB      IC      IN      VAB      VBC      VCA
 *   0.00    0.00    0.00    0.00    0.00     0.00     0.04
 *   ...  (many sample rows)
 *   =>
 *
 * Returns an array of { sample, IA, IB, IC, IN, VAB, VBC, VCA }.
 */
function parseEveResponse(text) {
  const lines = text.split(/\r?\n/);
  const rows  = [];

  // 1) Find the header line that contains the column labels
  //    e.g.  "IA   IB   IC   IN   VAB   VBC   VCA"
  let headers    = null;
  let headerIdx  = -1;

  for (let i = 0; i < lines.length; i++) {
    const tokens = lines[i].trim().split(/\s+/).filter(Boolean);
    // Detect header: all tokens are alphabetic labels (IA, IB, ICâ€¦)
    if (tokens.length >= 3 && tokens.every(t => /^[A-Za-z][A-Za-z0-9]*$/.test(t))) {
      // Confirm it's our expected header (contains at least IA or VAB)
      const upper = tokens.map(t => t.toUpperCase());
      if (upper.includes("IA") || upper.includes("VAB")) {
        headers   = upper;
        headerIdx = i;
        break;
      }
    }
  }

  // 2) Parse every subsequent data line
  if (headers && headerIdx >= 0) {
    let sampleNum = 1;
    for (let i = headerIdx + 1; i < lines.length; i++) {
      const line = lines[i].trim();
      if (!line || /^\s*=>\s*$/.test(line)) continue;

      const values = line.split(/\s+/).filter(Boolean);
      // Accept lines that have at least as many numeric tokens as headers
      if (values.length < headers.length) continue;
      if (!values.every(v => /^-?\d+(\.\d+)?$/.test(v))) continue;

      const row = { sample: sampleNum++ };
      for (let j = 0; j < headers.length; j++) {
        row[headers[j]] = values[j];
      }
      rows.push(row);
    }
  }

  // 3) Fallback: if we couldn't detect structured columns, push raw
  if (rows.length === 0) {
    // Parse every line that looks like all-numeric whitespace-separated
    let sampleNum = 1;
    for (const line of lines) {
      const trimmed = line.trim();
      if (!trimmed || /^\s*=>\s*$/.test(trimmed)) continue;
      const values = trimmed.split(/\s+/).filter(Boolean);
      if (values.length < 3) continue;
      if (!values.every(v => /^-?\d+(\.\d+)?$/.test(v))) continue;

      const row = { sample: sampleNum++ };
      const fallbackCols = ["IA", "IB", "IC", "IN", "VAB", "VBC", "VCA"];
      for (let j = 0; j < values.length && j < fallbackCols.length; j++) {
        row[fallbackCols[j]] = values[j];
      }
      rows.push(row);
    }
  }

  return rows;
}

// â”€â”€ Main Fetch Logic (sequential) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

async function evFetchReport() {
  const input  = document.getElementById("ev-event-num");
  const eveNum = parseInt(input?.value, 10);

  if (!eveNum || eveNum < 1) {
    if (typeof showToast === "function") showToast("Enter a valid event number", "error");
    return;
  }

  evAbort = false;
  _evSetFetchBtn(true);

  try {
    await _evEnsureConnection();
    _evSetStatus("fetching");

    const cmd      = "EVE " + eveNum + " R";
    const response = await _evSendCommand(cmd);
    const parsed   = parseEveResponse(response);

    if (parsed.length > 0) {
      _evAllRows.push(...parsed);
      _evUpdateTable();
    }

    _evSetStatus("done");
    console.log(`[EVE] EVE ${eveNum} fetched â€” ${_evAllRows.length} rows total`);

  } catch (err) {
    console.error("[EVE] fetch error:", err);
    _evSetStatus("error");
    if (typeof showToast === "function") {
      showToast(`Event fetch failed: ${err.message}`, "error");
    }
  } finally {
    _evSetFetchBtn(false);
  }
}

function _evUpdateTable() {
  if (evTable) {
    evTable.setData(_evAllRows);
  } else {
    initEvTable();
    if (evTable) evTable.setData(_evAllRows);
  }
  _evSetRecordCount(_evAllRows.length);
}

// â”€â”€ Disconnect helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

function _evDisconnectRaw() {
  if (evWs) {
    evWs.onopen = null; evWs.onclose = null; evWs.onerror = null; evWs.onmessage = null;
    try { evWs.close(); } catch (_) {}
    evWs = null;
  }
  _evResolve = null;
  _evReject  = null;
}

// â”€â”€ Public Actions â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

function evExportCSV() {
  if (evTable) evTable.download("csv", "event_reports.csv");
}

function evClearRecords() {
  evAbort = true;
  _evDisconnectRaw();
  _evAllRows = [];
  if (evTable) evTable.setData([]);
  _evSetRecordCount(0);
  _evSetStatus("idle");
  _evSetFetchBtn(false);
  console.log("[EVE] Records cleared");
}

function evDisconnect() {
  evAbort = true;
  _evDisconnectRaw();
  _evSetStatus("idle");
  _evSetFetchBtn(false);
}

// ============================================================
//  Time Sync Module
// ============================================================

/**
 * Send a JSON action over a transient WebSocket and return the parsed response.
 * Reuses the SER WebSocket URL (same C++ backend, same port).
 */
function _tsSendAction(actionObj) {
  return new Promise((resolve, reject) => {
    const url = buildSerWsUrl();
    const ws  = new WebSocket(url);
    ws.binaryType = 'arraybuffer';

    const timeout = setTimeout(() => {
      ws.close();
      reject(new Error("WebSocket timeout"));
    }, 8000);

    ws.onopen = () => {
      ws.send(JSON.stringify(actionObj));
    };

    ws.onmessage = (event) => {
      clearTimeout(timeout);
      try {
        // Ignore binary (TLV) frames â€” we only want text JSON responses
        if (typeof event.data === 'string') {
          const resp = JSON.parse(event.data);
          resolve(resp);
          ws.close();
        }
      } catch (e) {
        reject(e);
        ws.close();
      }
    };

    ws.onerror = () => {
      clearTimeout(timeout);
      reject(new Error("WebSocket error"));
    };

    ws.onclose = () => {
      clearTimeout(timeout);
    };
  });
}

function _tsSetStatus(status) {
  const el = document.getElementById("ts-conn-status");
  if (!el) return;
  const map = {
    idle:       { text: "ðŸŸ¡ Idle",        color: "#d97706" },
    fetching:   { text: "ðŸŸ¡ Fetchingâ€¦",   color: "#d97706" },
    syncing:    { text: "ðŸŸ¡ Syncingâ€¦",    color: "#d97706" },
    success:    { text: "ðŸŸ¢ Done",        color: "#16a34a" },
    error:      { text: "ðŸ”´ Error",       color: "#dc2626" }
  };
  const info = map[status] || map.idle;
  el.textContent = info.text;
  el.style.color = info.color;
}

function _tsShowResult(message, isError) {
  const el = document.getElementById("ts-result");
  if (!el) return;
  el.style.display = "";
  el.className = "ts-result " + (isError ? "ts-result--error" : "ts-result--success");
  el.textContent = message;
}

function _tsHideResult() {
  const el = document.getElementById("ts-result");
  if (el) el.style.display = "none";
}

/**
 * Fetch the relay's DATE response and display it.
 */
async function _tsReadRelayTime() {
  const relay = getCurrentRelay();
  if (!relay) return;
  const el = document.getElementById("ts-relay-time");
  const rawEl = document.getElementById("ts-relay-raw");
  if (el) el.textContent = "Loadingâ€¦";
  if (rawEl) rawEl.textContent = "";

  try {
    const resp = await _tsSendAction({ action: "read_relay_time", relay_id: String(relay.id) });
    if (resp.status === "success") {
      // The relay_time field contains the raw DATE response from the relay
      const raw = resp.relay_time || "â€”";
      // Try to extract a clean date/time from the response
      if (el) el.textContent = raw.trim().split("\n").find(l => l.match(/\d{2}\/\d{2}\/\d{2}/)) || raw.substring(0, 60);
      if (rawEl) rawEl.textContent = raw;
    } else {
      if (el) el.textContent = "Error";
      if (rawEl) rawEl.textContent = resp.error || "Unknown error";
    }
  } catch (e) {
    if (el) el.textContent = "Error";
    if (rawEl) rawEl.textContent = e.message;
  }
}

/**
 * Fetch PC time from the backend.
 */
async function _tsReadPcTime() {
  const el = document.getElementById("ts-pc-time");
  if (el) el.textContent = "Loadingâ€¦";

  try {
    const resp = await _tsSendAction({ action: "read_pc_time" });
    if (resp.status === "success") {
      if (el) el.textContent = resp.iso8601 || resp.dateTime || "â€”";
    } else {
      if (el) el.textContent = "Error";
    }
  } catch (e) {
    if (el) el.textContent = "Error";
  }
}

/**
 * Fetch SNTP time from the backend (which queries the NTP server).
 */
async function _tsReadSntpTime() {
  const server = (document.getElementById("ts-ntp-server") || {}).value || "pool.ntp.org";
  const el = document.getElementById("ts-sntp-time");
  if (el) el.textContent = "Loadingâ€¦";

  try {
    const resp = await _tsSendAction({ action: "read_sntp_time", server: server });
    if (resp.status === "success") {
      if (el) el.textContent = resp.iso8601 || resp.dateTime || "â€”";
    } else {
      if (el) el.textContent = "Error: " + (resp.error || "SNTP failed");
    }
  } catch (e) {
    if (el) el.textContent = "Error: " + e.message;
  }
}

/**
 * Refresh all three time displays.
 */
async function tsRefreshAll() {
  _tsHideResult();
  _tsSetStatus("fetching");
  try {
    await Promise.all([_tsReadRelayTime(), _tsReadPcTime(), _tsReadSntpTime()]);
    _tsSetStatus("success");
  } catch (_) {
    _tsSetStatus("error");
  }
}

/**
 * Sync the relay's clock with the PC time.
 */
async function tsSyncWithPcTime() {
  const relay = getCurrentRelay();
  if (!relay) return;
  _tsHideResult();
  _tsSetStatus("syncing");

  try {
    const resp = await _tsSendAction({
      action: "sync_relay_pc_time",
      relay_id: String(relay.id)
    });
    if (resp.status === "success") {
      _tsSetStatus("success");
      _tsShowResult("Relay time synced with PC time. Sent: " + (resp.set_date || "") + " / " + (resp.set_time || ""), false);
      // Refresh times after sync
      await tsRefreshAll();
    } else {
      _tsSetStatus("error");
      _tsShowResult("Sync failed: " + (resp.error || "Unknown error"), true);
    }
  } catch (e) {
    _tsSetStatus("error");
    _tsShowResult("Sync failed: " + e.message, true);
  }
}

/**
 * Sync the relay's clock with SNTP time.
 */
async function tsSyncWithSntpTime() {
  const relay = getCurrentRelay();
  if (!relay) return;
  const server = (document.getElementById("ts-ntp-server") || {}).value || "pool.ntp.org";
  _tsHideResult();
  _tsSetStatus("syncing");

  try {
    const resp = await _tsSendAction({
      action: "sync_relay_sntp_time",
      relay_id: String(relay.id),
      server: server
    });
    if (resp.status === "success") {
      _tsSetStatus("success");
      _tsShowResult("Relay time synced with SNTP (" + (resp.sntp_time || server) + "). Sent: " + (resp.set_date || "") + " / " + (resp.set_time || ""), false);
      // Refresh times after sync
      await tsRefreshAll();
    } else {
      _tsSetStatus("error");
      _tsShowResult("Sync failed: " + (resp.error || "Unknown error"), true);
    }
  } catch (e) {
    _tsSetStatus("error");
    _tsShowResult("Sync failed: " + e.message, true);
  }
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
    // Keep table alive â€” just disconnect WS; redraw on re-show
    serDataReceived = false;
  }

  function teardownRelayWord() {
    rwAbort = true;
    // Immediately reject any in-flight batch promise so its 2-minute timer
    // is cleared and fetchAllTAR() exits the catch block right away.
    if (_rwBatchReject) {
      const rej = _rwBatchReject;
      _rwBatchResolve = null;
      _rwBatchReject  = null;
      rej(new Error("aborted"));
    }
    _rwDisconnectRaw();
  }

  function teardownIO() {
    ioAbort = true;
    _ioDisconnectRaw();
  }

  function teardownEvent() {
    evAbort = true;
    _evDisconnectRaw();
  }

  function teardownSettings() {
    _setDisconnectRaw();
  }

  function teardownComtrade() {
    // Close COMTRADE WS if open; safe no-op if module not initialised.
    if (typeof _ctrDisconnectRaw === "function") _ctrDisconnectRaw();
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

  // --- References to Event Report section (static HTML in relay.html) ---
  const evSection    = document.getElementById("event-section");

  // --- References to Settings section (static HTML in relay.html) ---
  const setSection   = document.getElementById("settings-section");

  // --- References to COMTRADE section (static HTML in relay.html) ---
  const ctrSection   = document.getElementById("comtrade-section");

  // --- References to Time Sync section (static HTML in relay.html) ---
  const tsSection    = document.getElementById("timesync-section");

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
      if (!serWorkerConnected) connectSerWebSocket();
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
      // Auto-fetch TAR data (returns from cache instantly if available)
      fetchAllTAR();
    });
  }

  function hideRelayWordSection() {
    if (rwSection) rwSection.style.display = "none";
    container.style.display = "";
  }

  function showIOSection() {
    if (ioSection) ioSection.style.display = "";
    container.style.display = "none";

    // Reset filter to 'all' so clicking the section shows all I/O rows
    ioSetFilter('all');

    requestAnimationFrame(() => {
      if (!ioTable) {
        initIoTable();
      } else {
        ioTable.redraw(true);
      }
      // Force fresh fetch so all I/O rows are always loaded on section click
      _clearTarCache();
      fetchAllIO();
    });
  }

  function hideIOSection() {
    if (ioSection) ioSection.style.display = "none";
    container.style.display = "";
  }

  function showEventSection() {
    if (evSection) evSection.style.display = "";
    container.style.display = "none";

    requestAnimationFrame(() => {
      if (!evTable) {
        initEvTable();
      } else {
        evTable.redraw(true);
      }
    });
  }

  function hideEventSection() {
    if (evSection) evSection.style.display = "none";
    container.style.display = "";
  }

  function showSettingsSection() {
    if (setSection) setSection.style.display = "";
    container.style.display = "none";
    // Auto-fetch settings data when navigating to this section
    setFetchSettings();
  }

  function hideSettingsSection() {
    if (setSection) setSection.style.display = "none";
    container.style.display = "";
  }

  function showComtradeSection() {
    if (!ctrSection) return;
    ctrSection.style.display = "";
    container.style.display = "none";

    // Always show the panel for all relays
    const panel = document.getElementById("ctr-451-panel");
    if (panel) panel.style.display = "";

    // Render cached file list (if available)
    ctrRenderFileDirForCurrentRelay();
  }

  function hideComtradeSection() {
    if (ctrSection) ctrSection.style.display = "none";
    container.style.display = "";
  }

  function showTimeSyncSection() {
    if (tsSection) tsSection.style.display = "";
    container.style.display = "none";
    // Auto-refresh all times when section is opened
    if (typeof tsRefreshAll === "function") tsRefreshAll();
  }

  function hideTimeSyncSection() {
    if (tsSection) tsSection.style.display = "none";
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

    // Clean up previous Event Report resources when switching away
    if (section !== "event") {
      teardownEvent();
      hideEventSection();
    }

    // Clean up previous Settings resources when switching away
    if (section !== "settings") {
      teardownSettings();
      hideSettingsSection();
    }

    // Clean up previous COMTRADE resources when switching away
    if (section !== "comtrade") {
      teardownComtrade();
      hideComtradeSection();
    }

    // Hide Time Sync section when switching away
    if (section !== "timesync") {
      hideTimeSyncSection();
    }

    switch (section) {

      case "settings":
        showSettingsSection();
        break;

      case "comtrade":
        showComtradeSection();
        break;

      case "timesync":
        showTimeSyncSection();
        break;

      case "event":
        showEventSection();
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

  // Default load â€” SER tab is active
  loadSection("ser");

});
