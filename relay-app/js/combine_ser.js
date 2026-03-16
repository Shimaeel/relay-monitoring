/**
 * ============================================================
 *  combine_ser.js — Combined SER Table for Dashboard
 * ============================================================
 *
 *  Connects to the single shared WebSocket server (port 8765)
 *  and aggregates all SER records into a single Tabulator table.
 *  Each record carries relay_id / relay_name from the server,
 *  so a "Relay" column is populated from the TLV payload itself.
 *
 *  Dependencies: data.js (getRelays), Tabulator (CDN)
 */

// ============================================================
//  State
// ============================================================
let cserTable          = null;   // Tabulator instance
let cserWs             = null;   // Single WebSocket connection
let cserAutoRefresh    = null;   // Polling interval handle
let cserLastMessageAt  = 0;
let cserDataReceived   = false;
let cserKnownUids      = new Set(); // tracks _uid values already in cserTable

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

function _cserDecodeTlv(arrayBuffer) {
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
      if (fTlv.tag === 0x84) fields.relayId     = val;
      if (fTlv.tag === 0x85) fields.relayName   = val;
      rOff = fTlv.nextOffset;
    }

    const ts = fields.timestamp || "";
    let date = ts || "-";
    let time = "-";
    const sp = ts.indexOf(" ");
    if (sp !== -1) { date = ts.slice(0, sp); time = ts.slice(sp + 1); }

    const relayName = fields.relayName || "Unknown";
    const snoVal = Number.parseInt(fields.recordId, 10);
    const sno = Number.isNaN(snoVal) ? (fields.recordId || "-") : snoVal;
    records.push({
      _uid:    relayName + "_" + sno,
      relay:   relayName,
      sno:     sno,
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
  cserKnownUids.clear();
  data.forEach(r => cserKnownUids.add(r._uid));
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
    // Only append rows not already in the table — no full re-render
    const newRows = records.filter(r => !cserKnownUids.has(r._uid));
    if (newRows.length > 0) {
      cserTable.blockRedraw();
      cserTable.addData(newRows, false);   // false = append at bottom
      newRows.forEach(r => cserKnownUids.add(r._uid));
      cserTable.restoreRedraw();           // single flush, no flicker
    }
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
  if (cserWs && cserWs.readyState === WebSocket.OPEN) {
    el.textContent = "🟢 Connected";
    el.style.color = "#16a34a";
  } else {
    el.textContent = "🔴 Disconnected";
    el.style.color = "#dc2626";
  }
}

// ============================================================
//  Single Shared WebSocket Connection
// ============================================================

function _cserConnect() {
  if (cserWs && cserWs.readyState === WebSocket.OPEN) return;

  const host = "localhost";
  const port = 8765;
  const url  = `ws://${host}:${port}`;

  console.log("[CSER] Connecting to shared WS at", url);
  cserWs = new WebSocket(url);
  cserWs.binaryType = "arraybuffer";

  cserWs.onopen = () => {
    console.log("[CSER] Connected");
    _cserUpdateConnectionStatus();
    // Server sends initial data on connect — set timestamp so first poll doesn't fire immediately
    cserLastMessageAt = Date.now();
    _cserStartAutoRefresh();
  };

  cserWs.onmessage = async (event) => {
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
            const enriched = json.map(r => ({
              ...r,
              _uid:  (r.relay_name || "Unknown") + "_" + r.sno,
              relay: r.relay_name || "Unknown"
            }));
            _cserUpdateTable(enriched);
            cserLastMessageAt = Date.now();
          }
        } catch (_) {}
        return;
      }
      if (!bytes || bytes.length === 0) return;

      const exactBuffer = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
      const records = _cserDecodeTlv(exactBuffer);
      if (records.length > 0) {
        _cserUpdateTable(records);
        cserLastMessageAt = Date.now();
      }
    } catch (err) {
      console.error("[CSER] Decode error:", err);
    }
  };

  cserWs.onclose = () => {
    console.log("[CSER] Disconnected");
    _cserStopAutoRefresh();
    _cserUpdateConnectionStatus();
    // Auto-reconnect after 5 s
    setTimeout(() => {
      if (document.getElementById("combine-ser-table")) _cserConnect();
    }, 5000);
  };

  cserWs.onerror = (err) => {
    console.error("[CSER] WebSocket error:", err);
    _cserUpdateConnectionStatus();
  };
}

function _cserStartAutoRefresh() {
  _cserStopAutoRefresh();
  const rateEl = document.getElementById("cser-poll-rate");
  const rate = rateEl ? (parseInt(rateEl.value, 10) || 2000) : 2000;
  cserAutoRefresh = setInterval(() => {
    if (cserWs && cserWs.readyState === WebSocket.OPEN) cserWs.send("getData");
  }, rate);
}

function _cserStopAutoRefresh() {
  if (cserAutoRefresh) {
    clearInterval(cserAutoRefresh);
    cserAutoRefresh = null;
  }
}

// ============================================================
//  Public Actions (called from HTML onclick)
// ============================================================

function cserConnectAll() {
  _cserConnect();
}

function cserDisconnectAll() {
  _cserStopAutoRefresh();
  if (cserWs) {
    cserWs.onclose = null;
    cserWs.onerror = null;
    cserWs.close();
    cserWs = null;
  }
  _cserUpdateConnectionStatus();
}

function cserRefreshAll() {
  if (cserWs && cserWs.readyState === WebSocket.OPEN) cserWs.send("getData");
}

function cserExportCSV() {
  if (cserTable) cserTable.download("csv", "combine_ser_records.csv");
}

// ============================================================
//  Bootstrap
// ============================================================
document.addEventListener("DOMContentLoaded", () => {
  _cserInitTable([]);
  _cserConnect();

  // Update poll rate on change
  const pollEl = document.getElementById("cser-poll-rate");
  if (pollEl) {
    pollEl.addEventListener("change", () => {
      if (cserWs && cserWs.readyState === WebSocket.OPEN) {
        _cserStartAutoRefresh();
      }
    });
  }
});
