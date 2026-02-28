/**
 * ============================================================
 *  combine_ser.js — Combined SER Table for Dashboard
 * ============================================================
 *
 *  Connects to every relay's WebSocket server and aggregates
 *  all SER records into a single Tabulator table with a
 *  "Relay" column to identify the source device.
 *
 *  Dependencies: data.js (getRelays), Tabulator (CDN)
 */

// ============================================================
//  State
// ============================================================
let cserTable          = null;   // Tabulator instance
let cserConnections    = {};     // relayId → WebSocket
let cserAutoRefreshes  = {};     // relayId → interval handle
let cserLastMessageAt  = 0;
let cserDataReceived   = false;
let cserRowCounter     = 0;      // globally-unique row id

const _cserDecoder = new TextDecoder();

// ============================================================
//  ASN.1 BER / TLV Decoder  (same as sections.js)
// ============================================================

function _cserReadLength(buffer, offset) {
  const first = buffer[offset];
  if ((first & 0x80) === 0) return { length: first, nextOffset: offset + 1 };
  const count = first & 0x7f;
  if (count === 0 || offset + count >= buffer.length) throw new Error("Invalid BER length");
  let value = 0;
  for (let i = 0; i < count; i++) value = (value << 8) | buffer[offset + 1 + i];
  return { length: value, nextOffset: offset + 1 + count };
}

function _cserReadTlv(buffer, offset) {
  const tag = buffer[offset];
  const constructed = (tag & 0x20) !== 0;
  const lengthInfo = _cserReadLength(buffer, offset + 1);
  const valueStart = lengthInfo.nextOffset;
  return { tag, constructed, valueStart, length: lengthInfo.length, nextOffset: valueStart + lengthInfo.length };
}

function _cserDecodeTlv(arrayBuffer, relayName) {
  const buffer = new Uint8Array(arrayBuffer);
  let offset = 0;
  const topLevel = _cserReadTlv(buffer, offset);
  if (topLevel.tag !== 0x61) throw new Error("Unexpected TLV tag: 0x" + topLevel.tag.toString(16));

  const records = [];
  offset = topLevel.valueStart;
  const end = topLevel.valueStart + topLevel.length;

  while (offset < end) {
    const recordTlv = _cserReadTlv(buffer, offset);
    if (recordTlv.tag !== 0x30) { offset = recordTlv.nextOffset; continue; }

    const fields = {};
    let rOff = recordTlv.valueStart;
    const rEnd = recordTlv.valueStart + recordTlv.length;

    while (rOff < rEnd) {
      const fTlv = _cserReadTlv(buffer, rOff);
      const val = _cserDecoder.decode(buffer.slice(fTlv.valueStart, fTlv.valueStart + fTlv.length));
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
    cserRowCounter++;
    records.push({
      _uid:    cserRowCounter,
      relay:   relayName,
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
//  Status Formatter (same look as relay page)
// ============================================================

function _cserStatusFormatter(cell) {
  const v = cell.getValue();
  if (!v) return `<span class="ser-status--empty">—</span>`;
  if (v === "Asserted")   return `<span class="ser-status--assert">${v}</span>`;
  if (v === "Deasserted") return `<span class="ser-status--deassert">${v}</span>`;
  return `<span>${v}</span>`;
}

// ============================================================
//  Tabulator Init
// ============================================================

function _cserInitTable(data) {
  cserTable = new Tabulator("#combine-ser-table", {
    data: data,
    index: "_uid",
    layout: "fitColumns",
    pagination: true,
    paginationSize: 50,
    paginationSizeSelector: [10, 20, 50, 100],
    movableColumns: true,
    placeholder: "No Combined SER Records Available",
    height: "500px",
    columns: [
      {
        title: "S.No",
        field: "sno",
        widthGrow: 0,
        width: 80,
        hozAlign: "center",
        headerSort: true,
        sorter: "number",
        headerFilter: "input"
      },
      {
        title: "Relay",
        field: "relay",
        widthGrow: 0,
        minWidth: 120,
        hozAlign: "center",
        headerSort: true,
        headerFilter: "input"
      },
      {
        title: "Date",
        field: "date",
        widthGrow: 0,
        minWidth: 110,
        hozAlign: "center",
        headerSort: true,
        headerFilter: "input"
      },
      {
        title: "Time",
        field: "time",
        widthGrow: 0,
        minWidth: 120,
        hozAlign: "center",
        headerSort: true,
        headerFilter: "input"
      },
      {
        title: "Element",
        field: "element",
        widthGrow: 1,
        minWidth: 150,
        headerSort: true,
        headerFilter: "input"
      },
      {
        title: "State",
        field: "state",
        widthGrow: 0,
        minWidth: 100,
        hozAlign: "center",
        formatter: _cserStatusFormatter,
        headerSort: true,
        headerFilter: "input"
      }
    ],
    initialSort: [{ column: "sno", dir: "desc" }]
  });
  _cserUpdateStats();
}

function _cserUpdateTable(records) {
  if (!cserTable) {
    _cserInitTable(records);
  } else {
    cserTable.addData(records);
  }
  cserDataReceived = true;
  _cserUpdateStats();
}

function _cserUpdateStats() {
  const countEl  = document.getElementById("cser-record-count");
  const updateEl = document.getElementById("cser-last-update");
  if (cserTable && countEl) countEl.textContent = cserTable.getDataCount();
  if (updateEl) updateEl.textContent = new Date().toLocaleTimeString();
}

function _cserUpdateConnectionStatus() {
  const el = document.getElementById("cser-conn-status");
  if (!el) return;
  const relays = getRelays();
  const connectedCount = Object.values(cserConnections).filter(ws => ws && ws.readyState === WebSocket.OPEN).length;
  if (connectedCount === 0) {
    el.textContent = "🔴 Disconnected";
    el.style.color = "#dc2626";
  } else if (connectedCount < relays.length) {
    el.textContent = `🟡 ${connectedCount}/${relays.length} Connected`;
    el.style.color = "#d97706";
  } else {
    el.textContent = `🟢 ${connectedCount}/${relays.length} Connected`;
    el.style.color = "#16a34a";
  }
}

// ============================================================
//  WebSocket per Relay
// ============================================================

function _cserConnectRelay(relay) {
  const id = relay.id;
  if (cserConnections[id] && cserConnections[id].readyState === WebSocket.OPEN) return;

  const host = "localhost";
  const port = relay.wsPort || 8765;
  const url  = `ws://${host}:${port}`;

  console.log(`[CSER] Connecting to ${relay.name} at ${url}`);
  const ws = new WebSocket(url);
  ws.binaryType = "arraybuffer";
  cserConnections[id] = ws;

  ws.onopen = () => {
    console.log(`[CSER] Connected to ${relay.name}`);
    _cserUpdateConnectionStatus();
    _cserStartAutoRefresh(relay);
    try { ws.send("getData"); } catch (_) {}
  };

  ws.onmessage = async (event) => {
    try {
      let bytes;
      if (event.data instanceof Blob) {
        const ab = await event.data.arrayBuffer();
        bytes = new Uint8Array(ab);
      } else if (event.data instanceof ArrayBuffer) {
        bytes = new Uint8Array(event.data);
      } else {
        // JSON text fallback
        try {
          const json = JSON.parse(event.data);
          if (Array.isArray(json)) {
            const enriched = json.map(r => { cserRowCounter++; return { ...r, _uid: cserRowCounter, relay: relay.name }; });
            _cserUpdateTable(enriched);
            cserLastMessageAt = Date.now();
          }
        } catch (_) {}
        return;
      }
      if (!bytes || bytes.length === 0) return;

      const exactBuffer = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
      const records = _cserDecodeTlv(exactBuffer, relay.name);
      if (records.length > 0) {
        _cserUpdateTable(records);
        cserLastMessageAt = Date.now();
      }
    } catch (err) {
      console.error(`[CSER] Decode error (${relay.name}):`, err);
    }
  };

  ws.onclose = () => {
    console.log(`[CSER] Disconnected from ${relay.name}`);
    _cserStopAutoRefresh(relay);
    _cserUpdateConnectionStatus();
    // Auto-reconnect after 5 s
    setTimeout(() => {
      if (document.getElementById("combine-ser-table")) _cserConnectRelay(relay);
    }, 5000);
  };

  ws.onerror = (err) => {
    console.error(`[CSER] WebSocket error (${relay.name}):`, err);
    _cserUpdateConnectionStatus();
  };
}

function _cserStartAutoRefresh(relay) {
  _cserStopAutoRefresh(relay);
  const rateEl = document.getElementById("cser-poll-rate");
  const rate = rateEl ? (parseInt(rateEl.value, 10) || 2000) : 2000;
  cserAutoRefreshes[relay.id] = setInterval(() => {
    const ws = cserConnections[relay.id];
    if (ws && ws.readyState === WebSocket.OPEN) ws.send("getData");
  }, rate);
}

function _cserStopAutoRefresh(relay) {
  if (cserAutoRefreshes[relay.id]) {
    clearInterval(cserAutoRefreshes[relay.id]);
    delete cserAutoRefreshes[relay.id];
  }
}

// ============================================================
//  Public Actions (called from HTML onclick)
// ============================================================

function cserConnectAll() {
  const relays = getRelays();
  relays.forEach(r => _cserConnectRelay(r));
}

function cserDisconnectAll() {
  const relays = getRelays();
  relays.forEach(r => {
    _cserStopAutoRefresh(r);
    const ws = cserConnections[r.id];
    if (ws) {
      ws.onclose = null;
      ws.onerror = null;
      ws.close();
    }
    delete cserConnections[r.id];
  });
  _cserUpdateConnectionStatus();
}

function cserRefreshAll() {
  Object.values(cserConnections).forEach(ws => {
    if (ws && ws.readyState === WebSocket.OPEN) ws.send("getData");
  });
}

function cserExportCSV() {
  if (cserTable) cserTable.download("csv", "combine_ser_records.csv");
}

// ============================================================
//  Bootstrap
// ============================================================
document.addEventListener("DOMContentLoaded", () => {
  _cserInitTable([]);
  cserConnectAll();

  // Update poll rate on change
  const pollEl = document.getElementById("cser-poll-rate");
  if (pollEl) {
    pollEl.addEventListener("change", () => {
      getRelays().forEach(r => {
        if (cserConnections[r.id] && cserConnections[r.id].readyState === WebSocket.OPEN) {
          _cserStartAutoRefresh(r);
        }
      });
    });
  }
});
