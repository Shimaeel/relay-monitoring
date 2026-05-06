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
let cserGroupWindowSec = 60;        // time-window grouping (configurable from UI)

// Group color palette — must match sections.js / style.css
const CSER_GROUP_PALETTE_SIZE = 8;

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
      _uid:    relayName + "_" + sno + "_" + ts,
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
//  Relay Color Badge — distinct color per relay name
// ============================================================
// Uses a 10-slot palette assigned in first-seen order so colors stay stable
// across re-renders within a session.

const CSER_RELAY_PALETTE_SIZE = 10;
const _cserRelayColorMap = new Map();   // relayName → palette index 0..9

function _cserRelayColorIndex(relayName) {
  const key = String(relayName || 'Unknown');
  if (!_cserRelayColorMap.has(key)) {
    _cserRelayColorMap.set(key, _cserRelayColorMap.size % CSER_RELAY_PALETTE_SIZE);
  }
  return _cserRelayColorMap.get(key);
}

function _cserRelayFormatter(cell) {
  const v = cell.getValue();
  if (!v) return `<span class="cser-relay-badge cser-relay--empty">—</span>`;
  const idx = _cserRelayColorIndex(v);
  return `<span class="cser-relay-badge cser-relay-${idx}">${v}</span>`;
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
    selectableRows: true,
    groupBy: _cserGroupKey,
    groupHeader: _cserGroupHeader,
    rowFormatter: _cserRowColorFormatter,
    columns: [
      {
        title: "",
        formatter: "rowSelection",
        titleFormatter: "rowSelection",
        hozAlign: "center",
        headerSort: false,
        width: 40
      },
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
        formatter: _cserRelayFormatter,
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
      el.textContent = "⚠ Disconnected";
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
        // Tell the C++ backend to start the pipeline for every known relay
        // so SER data from all relays starts flowing into the combined table
        // automatically — no need to open each relay page individually.
        try {
          const relays = (typeof getRelays === 'function') ? getRelays() : [];
          relays.forEach(r => {
            cserWorker.postMessage({
              type: 'send',
              payload: JSON.stringify({ action: "start_relay", relay_id: String(r.id) })
            });
          });
          console.log(`[CSER] Sent start_relay for ${relays.length} relays`);
        } catch (e) {
          console.warn('[CSER] Failed to start relays:', e);
        }
        // Pull whatever is already in the backend DB right away.
        cserWorker.postMessage({ type: 'send', payload: 'getData' });
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
            _uid:  (r.relay_name || "Unknown") + "_" + r.sno + "_" + (r.timestamp || ""),
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

// ============================================================
//  Combined SER Time-Window Grouping
// ============================================================

function _cserParseTimestampMs(row) {
  if (!row || !row.date || !row.time) return NaN;
  const t = String(row.time).split('.')[0];
  const ms = Date.parse(`${row.date}T${t}`);
  return Number.isFinite(ms) ? ms : NaN;
}

function _cserBucketFor(row) {
  const ms = _cserParseTimestampMs(row);
  if (!Number.isFinite(ms)) return -1;
  const winMs = Math.max(1, cserGroupWindowSec) * 1000;
  return Math.floor(ms / winMs);
}

function _cserGroupKey(row) {
  if (row && row._imported) return "📁 Imported records";
  const bucket = _cserBucketFor(row);
  if (bucket < 0) return "⏱ Unknown time";
  const winMs = cserGroupWindowSec * 1000;
  const start = new Date(bucket * winMs);
  const end   = new Date(bucket * winMs + winMs - 1);
  const fmt = d => d.toISOString().replace('T', ' ').slice(0, 19);
  return `${fmt(start)} → ${fmt(end)}`;
}

function _cserGroupHeader(value, count) {
  return `<span class="ser-group-header__label">${value}</span> ` +
         `<span class="ser-group-header__count">(${count} event${count === 1 ? '' : 's'})</span>`;
}

function _cserRowColorFormatter(row) {
  const bucket = _cserBucketFor(row.getData());
  if (bucket < 0) return;
  const cls = `ser-group-${((bucket % CSER_GROUP_PALETTE_SIZE) + CSER_GROUP_PALETTE_SIZE) % CSER_GROUP_PALETTE_SIZE}`;
  row.getElement().classList.add(cls);
}

function cserApplyGroupWindow() {
  const input = document.getElementById('cser-group-window');
  if (!input) return;
  const val = parseInt(input.value, 10);
  if (!Number.isFinite(val) || val < 1) return;
  cserGroupWindowSec = val;
  if (cserTable) {
    cserTable.setGroupBy(_cserGroupKey);
    cserTable.redraw(true);
  }
}

// ============================================================
//  Combined SER Export — CSV / Excel / PDF (selected if any)
// ============================================================

function _cserGetExportRows() {
  if (!cserTable) return [];
  const selected = cserTable.getSelectedData();
  const rows = (selected && selected.length > 0) ? selected : cserTable.getData();
  return rows.map(r => ({
    "S.No":    r.sno,
    "Relay":   r.relay,
    "Date":    r.date,
    "Time":    r.time,
    "Element": r.element,
    "State":   r.state
  }));
}

function cserExportCSV() {
  if (!cserTable) return;
  const selected = cserTable.getSelectedRows();
  if (selected.length > 0) {
    cserTable.download("csv", "combine_ser_records.csv", {}, "selected");
  } else {
    cserTable.download("csv", "combine_ser_records.csv");
  }
}

async function cserExportExcel() {
  const rows = _cserGetExportRows();
  if (rows.length === 0) { alert('No rows to export.'); return; }
  try { await ensureXlsxLoaded(); }
  catch (e) { alert('Failed to load Excel library: ' + e.message); return; }
  const ws = XLSX.utils.json_to_sheet(rows);
  const wb = XLSX.utils.book_new();
  XLSX.utils.book_append_sheet(wb, ws, "Combined SER");
  XLSX.writeFile(wb, "combine_ser_records.xlsx");
}

// ============================================================
//  Import SOE Records — CSV / TSV / Excel
// ============================================================

let _cserImportParsedRows = [];   // last successfully parsed batch

function cserOpenImportModal() {
  const modal = document.getElementById('cser-import-modal');
  if (!modal) return;
  // Reset state
  document.getElementById('cser-import-device').value = '';
  document.getElementById('cser-import-file').value = '';
  document.getElementById('cser-import-preview').style.display = 'none';
  document.getElementById('cser-import-error').style.display = 'none';
  document.getElementById('cser-import-submit').disabled = true;
  _cserImportParsedRows = [];
  modal.style.display = 'flex';
}

function cserCloseImportModal() {
  const modal = document.getElementById('cser-import-modal');
  if (modal) modal.style.display = 'none';
}

function _cserImportShowError(msg) {
  const el = document.getElementById('cser-import-error');
  if (!el) return;
  el.textContent = msg;
  el.style.display = 'block';
  document.getElementById('cser-import-preview').style.display = 'none';
  document.getElementById('cser-import-submit').disabled = true;
}

function _cserImportShowPreview(rows) {
  const previewEl = document.getElementById('cser-import-preview');
  document.getElementById('cser-import-error').style.display = 'none';
  document.querySelector('.cser-import-modal__preview-count').textContent =
    `✓ Parsed ${rows.length} record${rows.length === 1 ? '' : 's'}`;
  const sampleEl = document.getElementById('cser-import-preview-rows');
  const sample = rows.slice(0, 3).map(r =>
    `${String(r.sno).padEnd(8)} ${r.date} ${r.time}  ${r.element}  ${r.state}`
  ).join('\n');
  sampleEl.textContent = sample || '(no rows)';
  previewEl.style.display = 'block';
  document.getElementById('cser-import-submit').disabled = rows.length === 0;
}

/**
 * Parse a delimited text blob (CSV or TSV).
 * Auto-detects header by checking if first row contains alphabetic column names.
 * Falls back to positional mapping (col 0 = sno, 1 = date, 2 = time, 3 = element, 4 = state).
 */
function _cserParseDelimited(text, delimiter) {
  const lines = text.split(/\r?\n/).map(l => l.trim()).filter(l => l.length > 0);
  if (lines.length === 0) return [];

  // Drop common SEL header artefacts ("=>", banner lines starting with non-data)
  const dataLines = lines.filter(l => !/^(=>|---|FID=|RELAY|\*+)/i.test(l));
  if (dataLines.length === 0) return [];

  const split = (line) => line.split(delimiter).map(c => c.trim());
  const firstCols = split(dataLines[0]);
  const looksLikeHeader = firstCols.some(c => /^(s\.?\s*no|sno|#|date|time|element|state|status|description)$/i.test(c));

  let headerMap = { sno: 0, date: 1, time: 2, element: 3, state: 4 };
  let rowsToParse = dataLines;

  if (looksLikeHeader) {
    rowsToParse = dataLines.slice(1);
    const norm = firstCols.map(c => c.toLowerCase().replace(/[^a-z]/g, ''));
    const findIdx = (...keys) => {
      for (const k of keys) {
        const i = norm.indexOf(k);
        if (i !== -1) return i;
      }
      return -1;
    };
    headerMap.sno     = findIdx('sno', 'sno', 'no', 'recordid', 'id') ;
    headerMap.date    = findIdx('date');
    headerMap.time    = findIdx('time');
    headerMap.element = findIdx('element', 'description');
    headerMap.state   = findIdx('state', 'status');
    if (headerMap.sno === -1) headerMap.sno = 0;
    if (headerMap.date === -1) headerMap.date = 1;
    if (headerMap.time === -1) headerMap.time = 2;
    if (headerMap.element === -1) headerMap.element = 3;
    if (headerMap.state === -1) headerMap.state = 4;
  }

  return rowsToParse.map(line => {
    const cols = split(line);
    return {
      sno:     cols[headerMap.sno]     ?? '',
      date:    cols[headerMap.date]    ?? '',
      time:    cols[headerMap.time]    ?? '',
      element: cols[headerMap.element] ?? '',
      state:   cols[headerMap.state]   ?? ''
    };
  });
}

/** Convert MM/DD/YYYY to YYYY-MM-DD; pass-through ISO. */
function _cserNormaliseDate(d) {
  if (!d) return d;
  const s = String(d).trim();
  // Already ISO
  if (/^\d{4}-\d{2}-\d{2}$/.test(s)) return s;
  // MM/DD/YYYY or M/D/YYYY
  const m = s.match(/^(\d{1,2})[\/\-](\d{1,2})[\/\-](\d{2,4})$/);
  if (m) {
    const yyyy = m[3].length === 2 ? '20' + m[3] : m[3];
    return `${yyyy}-${m[1].padStart(2, '0')}-${m[2].padStart(2, '0')}`;
  }
  return s;
}

/** Build a combined-SER row in the shape _cserUpdateTable expects. */
function _cserBuildImportedRow(rawRow, deviceName) {
  const date = _cserNormaliseDate(rawRow.date);
  const time = String(rawRow.time || '').trim();
  const snoVal = Number.parseInt(rawRow.sno, 10);
  const sno = Number.isNaN(snoVal) ? (rawRow.sno || '-') : snoVal;
  const ts = `${date} ${time}`;
  return {
    _uid:      `${deviceName}_${sno}_${ts}_imported`,
    _imported: true,
    relay:     deviceName,
    sno:       sno,
    date:      date || '-',
    time:      time || '-',
    element:   rawRow.element || '-',
    state:     rawRow.state || ''
  };
}

async function cserPreviewImportFile() {
  const fileInput = document.getElementById('cser-import-file');
  const file = fileInput.files && fileInput.files[0];
  if (!file) return;

  try {
    let rawRows = [];
    const name = file.name.toLowerCase();

    if (name.endsWith('.xlsx') || name.endsWith('.xls')) {
      await ensureXlsxLoaded();
      const buf = await file.arrayBuffer();
      const wb = XLSX.read(buf, { type: 'array' });
      const sheet = wb.Sheets[wb.SheetNames[0]];
      const aoa = XLSX.utils.sheet_to_json(sheet, { header: 1, defval: '', raw: false });
      // Convert to delimited text and reuse the parser (consistent header detection)
      const tsv = aoa.map(r => r.join('\t')).join('\n');
      rawRows = _cserParseDelimited(tsv, '\t');
    } else if (name.endsWith('.csv')) {
      const text = await file.text();
      rawRows = _cserParseDelimited(text, ',');
    } else {
      // .tsv / .txt — assume tab-separated
      const text = await file.text();
      rawRows = _cserParseDelimited(text, '\t');
    }

    if (rawRows.length === 0) {
      _cserImportShowError('No data rows found in this file.');
      return;
    }

    _cserImportParsedRows = rawRows;
    _cserImportShowPreview(rawRows);
  } catch (err) {
    console.error('[CSER] Import parse error:', err);
    _cserImportShowError('Failed to parse file: ' + (err.message || err));
  }
}

function cserSubmitImport() {
  const device = document.getElementById('cser-import-device').value.trim();
  if (!device) {
    _cserImportShowError('Device Name is required.');
    return;
  }
  if (_cserImportParsedRows.length === 0) {
    _cserImportShowError('No parsed records to import. Choose a file first.');
    return;
  }

  const records = _cserImportParsedRows.map(r => _cserBuildImportedRow(r, device));

  if (!cserTable) {
    _cserInitTable(records);
    records.forEach(r => cserKnownUids.add(r._uid));
  } else {
    const fresh = records.filter(r => !cserKnownUids.has(r._uid));
    cserTable.blockRedraw();
    cserTable.addData(fresh, false);
    fresh.forEach(r => cserKnownUids.add(r._uid));
    cserTable.restoreRedraw();
  }
  cserDataReceived = true;
  _cserUpdateStats();
  console.log(`[CSER] Imported ${records.length} records for device "${device}"`);
  cserCloseImportModal();
}

async function cserExportPDF() {
  const rows = _cserGetExportRows();
  if (rows.length === 0) { alert('No rows to export.'); return; }
  try { await ensureJsPdfLoaded(); }
  catch (e) { alert('Failed to load PDF library: ' + e.message); return; }
  const { jsPDF } = window.jspdf;
  const doc = new jsPDF({ orientation: 'landscape' });
  const headers = Object.keys(rows[0]);
  const body    = rows.map(r => headers.map(h => r[h]));
  doc.setFontSize(14);
  doc.text("Combined SER Records", 14, 14);
  doc.autoTable({
    head: [headers],
    body: body,
    startY: 20,
    styles: { fontSize: 8 },
    headStyles: { fillColor: [60, 90, 153] }
  });
  doc.save("combine_ser_records.pdf");
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
