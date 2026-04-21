/* COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com) */

/**
 * ============================================================
 *  comtrade_live.js — Live COMTRADE Viewer (FSM-driven, no UI)
 * ============================================================
 *
 *  Subscribes to the shared WebSocket server (port 8765) and
 *  listens for two server-pushed message types, produced by the
 *  backend FILE DIR EVENTS collector:
 *
 *    COMTRADE_DIR:<json>     – directory listing (metadata only)
 *    COMTRADE_EVENT:<json>   – one complete event (cfg + dat text)
 *
 *  Renders each event as a card with Download .cfg / Download .dat
 *  buttons and client-side filter inputs.
 *
 *  Used on two pages:
 *    - index.html (dashboard)  → shows ALL relays’ events
 *    - relay.html              → shows only the current relay’s events
 *      (filter applied via ?id=<relayId> in the URL).
 *
 *  There are deliberately no fetch buttons: everything flows from
 *  the backend state machine.
 * ============================================================
 */

(function () {
  "use strict";

  // ------------------------------------------------------------
  //  Config / state
  // ------------------------------------------------------------
  const CTL_PREFIX_EVT = "COMTRADE_EVENT:";
  const CTL_PREFIX_DIR = "COMTRADE_DIR:";
  const WS_URL         = "ws://localhost:8765";
  const RECONNECT_MS   = 5000;

  /** @type {WebSocket|null} */
  let ws = null;
  let reconnectTimer = null;

  /** key = `${relayId}#${num}` */
  const events = new Map();

  // Options passed by the page via initComtradeLive()
  let opts = {
    containerId: "comtrade-live-list",
    countId:     null,        // element id whose textContent = event count
    scopeRelayId: null,       // if set, only this relay’s events are shown
    showRelayColumn: true,    // dashboard = true, relay page = false
    filtersPrefix: "ctl",     // id prefix for filter inputs (date/name/sub)
  };

  // ------------------------------------------------------------
  //  DOM helpers
  // ------------------------------------------------------------
  function el(id) { return document.getElementById(id); }

  function escHtml(s) {
    const d = document.createElement("div");
    d.textContent = s == null ? "" : String(s);
    return d.innerHTML;
  }

  function dlBlob(filename, text) {
    const blob = new Blob([text || ""], { type: "text/plain" });
    const url  = URL.createObjectURL(blob);
    const a    = document.createElement("a");
    a.href = url; a.download = filename;
    document.body.appendChild(a); a.click();
    document.body.removeChild(a); URL.revokeObjectURL(url);
  }

  // ------------------------------------------------------------
  //  Minimal STORE-method ZIP writer (no deps, no compression)
  // ------------------------------------------------------------
  const CRC_TABLE = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
      t[n] = c >>> 0;
    }
    return t;
  })();

  function crc32(bytes) {
    let c = 0xffffffff;
    for (let i = 0; i < bytes.length; i++)
      c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  }

  /**
   * Build a ZIP (store/no-compression) Blob from [{name, text}].
   * Good enough for small COMTRADE text files — any unzipper can read it.
   */
  function buildZip(entries) {
    const enc = new TextEncoder();
    const chunks = [];
    const central = [];
    let offset = 0;

    const u16 = (v) => new Uint8Array([v & 0xff, (v >>> 8) & 0xff]);
    const u32 = (v) => new Uint8Array([
      v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff
    ]);
    const concat = (arrs) => {
      let len = 0;
      arrs.forEach(a => (len += a.length));
      const out = new Uint8Array(len);
      let p = 0;
      arrs.forEach(a => { out.set(a, p); p += a.length; });
      return out;
    };

    entries.forEach(e => {
      const nameBytes = enc.encode(e.name);
      const dataBytes = enc.encode(e.text || "");
      const crc = crc32(dataBytes);
      const size = dataBytes.length;

      // Local file header (30 bytes + name + data)
      const lfh = concat([
        u32(0x04034b50),   // signature
        u16(20),           // version needed
        u16(0),            // flags
        u16(0),            // method = store
        u16(0), u16(0),    // mod time / date
        u32(crc),
        u32(size),         // compressed
        u32(size),         // uncompressed
        u16(nameBytes.length),
        u16(0),            // extra length
        nameBytes,
        dataBytes
      ]);
      chunks.push(lfh);

      // Central directory entry
      const cdh = concat([
        u32(0x02014b50),
        u16(20), u16(20),  // version made by / needed
        u16(0),            // flags
        u16(0),            // method
        u16(0), u16(0),    // time / date
        u32(crc),
        u32(size), u32(size),
        u16(nameBytes.length),
        u16(0), u16(0),    // extra / comment
        u16(0), u16(0),    // disk / internal attrs
        u32(0),            // external attrs
        u32(offset),
        nameBytes
      ]);
      central.push(cdh);
      offset += lfh.length;
    });

    const cdOffset = offset;
    let cdSize = 0;
    central.forEach(c => { chunks.push(c); cdSize += c.length; });

    // End of central directory
    const eocd = concat([
      u32(0x06054b50),
      u16(0), u16(0),
      u16(entries.length), u16(entries.length),
      u32(cdSize),
      u32(cdOffset),
      u16(0)
    ]);
    chunks.push(eocd);

    return new Blob(chunks, { type: "application/zip" });
  }

  function dlZip(filename, entries) {
    const blob = buildZip(entries);
    const url  = URL.createObjectURL(blob);
    const a    = document.createElement("a");
    a.href = url; a.download = filename;
    document.body.appendChild(a); a.click();
    document.body.removeChild(a); URL.revokeObjectURL(url);
  }

  // ------------------------------------------------------------
  //  Filtering
  // ------------------------------------------------------------
  function readFilters() {
    const p = opts.filtersPrefix;
    return {
      relay:      (el(p + "-f-relay")      || {}).value?.trim() || "",
      substation: (el(p + "-f-substation") || {}).value?.trim() || "",
      date:       (el(p + "-f-date")       || {}).value?.trim() || "",
      name:       (el(p + "-f-name")       || {}).value?.trim().toLowerCase() || "",
    };
  }

  function matches(ev, f) {
    if (opts.scopeRelayId && ev.relayId !== opts.scopeRelayId) return false;
    if (f.relay      && !ev.relayName?.toLowerCase().includes(f.relay.toLowerCase())) return false;
    if (f.substation && !ev.substation?.toLowerCase().includes(f.substation.toLowerCase())) return false;
    if (f.date       && !ev.date?.includes(f.date)) return false;
    if (f.name       && !(ev.name || "").toLowerCase().includes(f.name)) return false;
    return true;
  }

  // Return the content (or null) we can supply for a given filename’s extension.
  function contentFor(ev, ext) {
    const e = (ext || "").toUpperCase();
    if (e === "CFG") return ev.cfg || null;
    if (e === "DAT") return ev.dat || null;
    // CEV / HIS are SEL-native raw files; not fetched by the collector yet.
    return null;
  }

  function iconFor(ext) {
    const e = (ext || "").toUpperCase();
    if (e === "CFG") return "📄";
    if (e === "DAT") return "📊";
    if (e === "CEV") return "⚡";
    if (e === "HIS") return "📜";
    return "📁";
  }

  // ------------------------------------------------------------
  //  Render — one ROW per physical file listed in FILE DIR EVENTS
  // ------------------------------------------------------------
  function render() {
    const box = el(opts.containerId);
    if (!box) return;

    const f = readFilters();
    const visibleEvents = Array.from(events.values())
      .filter(ev => matches(ev, f))
      .sort((a, b) => {
        const k = (ev) => (ev.date || "") + " " + (ev.time || "");
        const kb = k(b), ka = k(a);
        if (ka !== kb) return kb < ka ? -1 : 1;
        return b.num - a.num;
      });

    // Flatten into per-file rows
    const fileRows = [];
    visibleEvents.forEach(ev => {
      const list = (ev.filenames && ev.filenames.length)
        ? ev.filenames
        : [{ name: ev.name || ("event_" + ev.num), ext: (ev.name || "").split(".").pop() }];
      list.forEach(fn => {
        const ext = (fn.ext || (fn.name || "").split(".").pop() || "").toUpperCase();
        fileRows.push({
          ev,
          filename: fn.name,
          ext,
          content: contentFor(ev, ext)
        });
      });
    });

    if (opts.countId) {
      const c = el(opts.countId);
      if (c) c.textContent = String(fileRows.length);
    }

    if (fileRows.length === 0) {
      box.innerHTML =
        '<div class="set-empty">No COMTRADE events yet — ' +
        'backend collector will auto-push them as soon as the relay reports new events.</div>';
      return;
    }

    const html = fileRows.map((r, i) => {
      const ev = r.ev;
      const relayBadge = opts.showRelayColumn
        ? `<span class="ctf-file-card__badge ctf-file-card__badge--cfg">${escHtml(ev.relayName || ev.relayId)}</span>`
        : "";
      const disabled = r.content ? "" : "disabled";
      const tip = r.content ? "" : 'title="Raw file not collected — only .CFG / .DAT are auto-fetched"';
      return `
        <div class="ctf-file-card">
          <div class="ctf-file-card__header">
            <div class="ctf-file-card__info">
              <span class="ctf-file-card__icon">${iconFor(r.ext)}</span>
              <span class="ctf-file-card__name">${escHtml(r.filename)}</span>
              <span class="ctf-file-card__badge">${escHtml(r.ext)}</span>
              ${relayBadge}
              ${ev.substation ? `<span class="ctf-file-card__badge">${escHtml(ev.substation)}</span>` : ""}
              ${ev.bay ? `<span class="ctf-file-card__badge">${escHtml(ev.bay)}</span>` : ""}
            </div>
            <div class="ctf-file-card__meta">
              <span>#${ev.num}</span>
              <span>${escHtml(ev.date || "")}</span>
              <span>${escHtml(ev.time || "")}</span>
            </div>
          </div>
          <div class="ctf-file-card__actions">
            <button class="btn btn--primary btn--sm" data-idx="${i}" ${disabled} ${tip}>
              ⬇ Download
            </button>
          </div>
        </div>
      `;
    }).join("");

    box.innerHTML = html;

    // Delegate button clicks — each row downloads its ONE file.
    box.querySelectorAll("button[data-idx]").forEach(btn => {
      btn.addEventListener("click", () => {
        const idx = parseInt(btn.getAttribute("data-idx"), 10);
        const row = fileRows[idx];
        if (!row || !row.content) return;
        dlBlob(row.filename, row.content);
      });
    });
  }

  // ------------------------------------------------------------
  //  WebSocket
  // ------------------------------------------------------------
  function connect() {
    try { if (ws) ws.close(); } catch (_) {}
    ws = new WebSocket(WS_URL);

    ws.onopen = () => {
      console.log("[CTR-LIVE] WS connected", WS_URL);
    };

    ws.onmessage = (msg) => {
      const d = msg.data;
      if (typeof d !== "string") return;

      if (d.startsWith(CTL_PREFIX_EVT)) {
        const json = d.slice(CTL_PREFIX_EVT.length);
        let ev;
        try { ev = JSON.parse(json); } catch (_) {
          console.warn("[CTR-LIVE] bad COMTRADE_EVENT JSON"); return;
        }
        const key = ev.relayId + "#" + ev.num;
        // Preserve any filenames[] already gathered from COMTRADE_DIR
        const prev = events.get(key);
        if (prev && prev.filenames && !ev.filenames) {
          ev.filenames = prev.filenames;
        }
        events.set(key, ev);
        render();
      }
      else if (d.startsWith(CTL_PREFIX_DIR)) {
        const json = d.slice(CTL_PREFIX_DIR.length);
        let dir;
        try { dir = JSON.parse(json); } catch (_) {
          console.warn("[CTR-LIVE] bad COMTRADE_DIR JSON"); return;
        }
        // Group DIR entries by event num so each num gets filenames[]
        // We only surface .CFG / .DAT — those are the only files the
        // backend collector actually fetches content for.
        const byNum = new Map();
        (dir.files || []).forEach(meta => {
          const ext = (meta.name || "").split(".").pop().toUpperCase();
          if (ext !== "CFG" && ext !== "DAT") return;
          if (!byNum.has(meta.num)) {
            byNum.set(meta.num, {
              num: meta.num,
              date: meta.date,
              time: meta.time,
              name: meta.name,
              filenames: []
            });
          }
          const g = byNum.get(meta.num);
          g.filenames.push({ name: meta.name, ext });
          if (ext === "CFG") g.name = meta.name; // prefer CFG as label
        });

        byNum.forEach((g, num) => {
          const key = dir.relayId + "#" + num;
          const existing = events.get(key) || {};
          events.set(key, Object.assign({}, existing, {
            relayId:    dir.relayId,
            relayName:  dir.relayName,
            substation: dir.substation,
            bay:        dir.bay,
            num:        num,
            name:       existing.name || g.name,
            date:       existing.date || g.date,
            time:       existing.time || g.time,
            filenames:  g.filenames,
            cfg:        existing.cfg || "",
            dat:        existing.dat || ""
          }));
        });
        render();
      }
      // other message types silently ignored
    };

    ws.onclose = () => {
      console.log("[CTR-LIVE] WS closed — reconnect in", RECONNECT_MS, "ms");
      if (reconnectTimer) clearTimeout(reconnectTimer);
      reconnectTimer = setTimeout(connect, RECONNECT_MS);
    };

    ws.onerror = (e) => {
      console.warn("[CTR-LIVE] WS error", e);
      try { ws.close(); } catch (_) {}
    };
  }

  // ------------------------------------------------------------
  //  Export currently-filtered files as a single COMTRADE .zip
  // ------------------------------------------------------------
  function exportFiltered() {
    const f = readFilters();
    const visible = Array.from(events.values()).filter(ev => matches(ev, f));
    const entries = [];
    visible.forEach(ev => {
      // One folder per event so .cfg + .dat stay paired inside the zip
      const folder = `${ev.relayName || ev.relayId}_event_${ev.num}/`;
      if (ev.cfg) entries.push({ name: folder + (fnByExt(ev, "CFG") || `event_${ev.num}.cfg`), text: ev.cfg });
      if (ev.dat) entries.push({ name: folder + (fnByExt(ev, "DAT") || `event_${ev.num}.dat`), text: ev.dat });
    });

    if (entries.length === 0) {
      alert("No COMTRADE content available to export yet.\n" +
            "Wait for the backend collector to push .cfg / .dat, " +
            "or relax the filters.");
      return;
    }

    const stamp = new Date().toISOString().replace(/[:.]/g, "-").slice(0, 19);
    dlZip(`comtrade_export_${stamp}.zip`, entries);
  }

  function fnByExt(ev, ext) {
    if (!ev.filenames) return null;
    const hit = ev.filenames.find(x => (x.ext || "").toUpperCase() === ext);
    return hit ? hit.name : null;
  }

  // ------------------------------------------------------------
  //  Public API
  // ------------------------------------------------------------
  window.initComtradeLive = function (userOpts) {
    opts = Object.assign(opts, userOpts || {});

    // Wire up filter inputs if present
    const p = opts.filtersPrefix;
    ["relay", "substation", "date", "name"].forEach(k => {
      const inp = el(p + "-f-" + k);
      if (inp) inp.addEventListener("input", render);
    });

    // Wire up export button if present
    const exportBtn = el(opts.filtersPrefix + "-export-btn");
    if (exportBtn) exportBtn.addEventListener("click", exportFiltered);

    render();        // empty state
    connect();       // start WS
  };

  // Expose for manual/programmatic use
  window.comtradeLiveExport = exportFiltered;

})();
