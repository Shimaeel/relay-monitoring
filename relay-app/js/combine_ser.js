/**
 * ============================================================
 *  combine_ser.js — Combined SER Table for Dashboard
 * ============================================================
 *
 *  Uses a Web Worker + SharedArrayBuffer ring buffer to receive
 *  SER records from the shared WebSocket server (port 8765).
 *  Binary TLV data flows through the ring buffer (zero-copy),
 *  while text messages are forwarded via postMessage.
 *
 *  Dependencies: ring_buffer.js, ser_worker.js, data.js, Tabulator
 */

// ============================================================
//  State
// ============================================================
let cserTable          = null;   // Tabulator instance
let cserWorker         = null;   // SER Web Worker
let cserRing           = null;   // SharedArrayBuffer ring buffer handle
let cserReaderId       = -1;     // Ring buffer reader slot
let cserReaderRunning  = false;  // Ring reader loop active
let cserConnected      = false;  // Worker WS connected flag
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
  if (cserConnected) {
    el.textContent = "🟢 Connected";
    el.style.color = "#16a34a";
  } else {
    el.textContent = "🔴 Disconnected";
    el.style.color = "#dc2626";
  }
}

// ============================================================
//  Worker + Ring Buffer Connection
// ============================================================

function _cserSetupWorker() {
  if (cserWorker) return;

  if (typeof SharedArrayBuffer === 'undefined') {
    console.error('[CSER] SharedArrayBuffer not available — enable Cross-Origin Isolation headers.');
    // Show clear error in UI instead of staying on "Connecting"
    const el = document.getElementById("cser-conn-status");
    if (el) {
      el.textContent = "❌ SharedArrayBuffer unavailable — serve via serve.ps1";
      el.style.color = "#dc2626";
    }
    return;
  }

  cserRing = createRingBuffer(512 * 1024);  // 512 KB ring
  cserWorker = new Worker('js/ser_worker.js');

  cserWorker.postMessage({
    type: 'init',
    buffer: cserRing.buffer,
    capacity: cserRing.capacity
  });

  cserWorker.onmessage = (event) => {
    const msg = event.data;
    if (!msg || !msg.type) return;

    if (msg.type === 'ws_status') {
      cserConnected = msg.status === 'connected';
      _cserUpdateConnectionStatus();
      if (cserConnected) {
        console.log('[CSER] Connected via worker');
        cserLastMessageAt = Date.now();
        _cserStartAutoRefresh();
      } else if (msg.status === 'disconnected') {
        console.log('[CSER] Disconnected');
        _cserStopAutoRefresh();
      }
    } else if (msg.type === 'ws_text') {
      // JSON text fallback (non-binary path)
      try {
        const json = JSON.parse(msg.data);
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
    } else if (msg.type === 'error') {
      console.error('[CSER] Worker error:', msg.message);
    }
  };

  cserWorker.onerror = (event) => {
    console.error('[CSER] Worker error:', event.message);
  };

  _cserStartRingReader();
}

function _cserStartRingReader() {
  if (cserReaderRunning || !cserRing) return;

  cserReaderId = cserRing.registerReader();
  if (cserReaderId < 0) {
    console.error('[CSER] All ring reader slots full');
    return;
  }

  cserReaderRunning = true;
  let lastSignal = Atomics.load(cserRing.header, 1);

  const pump = async () => {
    while (cserReaderRunning) {
      // Wait for signal from writer
      if (typeof Atomics.waitAsync === 'function') {
        const result = Atomics.waitAsync(cserRing.header, 1, lastSignal);
        if (result && result.value && typeof result.value.then === 'function') {
          await result.value;
        } else {
          await new Promise(r => setTimeout(r, 50));
        }
      } else {
        await new Promise(r => setTimeout(r, 50));
      }

      const current = Atomics.load(cserRing.header, 1);
      if (current === lastSignal) continue;
      lastSignal = current;

      // Drain all available payloads
      let payload = cserRing.readPayload(cserReaderId);
      while (payload) {
        try {
          const buf = payload.buffer.slice(payload.byteOffset, payload.byteOffset + payload.byteLength);
          const records = _cserDecodeTlv(buf);
          if (records.length > 0) {
            _cserUpdateTable(records);
            cserLastMessageAt = Date.now();
          }
        } catch (err) {
          console.error('[CSER] Ring decode error:', err);
        }
        payload = cserRing.readPayload(cserReaderId);
      }
    }
  };

  pump();
}

function _cserStopReading() {
  cserReaderRunning = false;
  if (cserRing && cserReaderId >= 0) {
    cserRing.unregisterReader(cserReaderId);
    cserReaderId = -1;
  }
}

function _cserStartAutoRefresh() {
  _cserStopAutoRefresh();
  const rateEl = document.getElementById("cser-poll-rate");
  const rate = rateEl ? (parseInt(rateEl.value, 10) || 2000) : 2000;
  cserAutoRefresh = setInterval(() => {
    if (cserWorker && cserConnected) {
      cserWorker.postMessage({ type: 'send', payload: 'getData' });
    }
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
  _cserSetupWorker();
  if (cserWorker) {
    cserWorker.postMessage({ type: 'connect', wsUrl: 'ws://localhost:8765' });
  }
}

function cserDisconnectAll() {
  _cserStopAutoRefresh();
  _cserStopReading();
  if (cserWorker) {
    cserWorker.postMessage({ type: 'disconnect' });
  }
  cserConnected = false;
  _cserUpdateConnectionStatus();
}

function cserRefreshAll() {
  if (cserWorker && cserConnected) {
    cserWorker.postMessage({ type: 'send', payload: 'getData' });
  }
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
      if (cserConnected) _cserStartAutoRefresh();
    });
  }
});
