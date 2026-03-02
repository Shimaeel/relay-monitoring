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

  // --- References to SER section (static HTML in relay.html) ---
  const serSection   = document.getElementById("ser-section");
  const serHostInput  = document.getElementById("ser-ws-host");
  const serPortInput  = document.getElementById("ser-ws-port");
  const serPollInput  = document.getElementById("ser-poll-rate");

  // Pre-fill SER config inputs from relay context
  if (serHostInput) serHostInput.value = defaultHost;
  if (serPortInput) serPortInput.value = defaultPort;

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

  function loadSection(section) {

    // Clean up previous SER resources when switching away
    if (section !== "ser") {
      teardownSer();
      hideSerSection();
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
        container.innerHTML = `
          <h2>🔌 Inputs / Outputs</h2>
          <p>INxxx / OUTxxx real-time state monitoring.</p>
        `;
        break;

      case "relayword":
        container.innerHTML = `
          <h2>📊 Relay Word Status</h2>
          <p>Bit-level relay word breakdown displayed here.</p>
        `;
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
