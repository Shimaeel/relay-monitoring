/**
 * ============================================================
 *  comtrade_files.js — COMTRADE Files Page
 * ============================================================
 *
 *  Standalone page script for comtrade.html.
 *  Lets the user select a relay, fetch CTR C / CTR D files,
 *  and displays all fetched files as cards with download buttons.
 *
 *  Dependencies: data.js (getRelays)
 * ============================================================
 */

// ============================================================
//  State
// ============================================================
let ctfWs        = null;
let _ctfResolve  = null;
let _ctfReject   = null;

/** @type {Array<{id:number, relay:string, relayId:string, type:string, fileNum:number, text:string, fetchedAt:string}>} */
let _ctfFiles = [];
let _ctfNextId = 1;

// ============================================================
//  DOM Helpers
// ============================================================

function _ctfSetStatus(status) {
  const el = document.getElementById("ctf-conn-status");
  if (!el) return;
  const map = {
    idle:       { text: "\ud83d\udfe1 Idle",            color: "#d97706" },
    connecting: { text: "\ud83d\udfe1 Connecting\u2026", color: "#d97706" },
    fetching:   { text: "\ud83d\udd35 Fetching\u2026",   color: "#2563eb" },
    done:       { text: "\ud83d\udfe2 Done",             color: "#16a34a" },
    error:      { text: "\ud83d\udd34 Error",            color: "#dc2626" }
  };
  const info = map[status] || map.idle;
  el.textContent = info.text;
  el.style.color = info.color;
}

function _ctfSetBtns(disabled) {
  ["ctf-c-fetch-btn", "ctf-d-fetch-btn", "ctf-both-fetch-btn"].forEach(function (id) {
    var btn = document.getElementById(id);
    if (!btn) return;
    btn.disabled = disabled;
    btn.style.opacity = disabled ? "0.5" : "1";
    btn.style.pointerEvents = disabled ? "none" : "";
  });
}

function _ctfEscHtml(s) {
  var d = document.createElement("div");
  d.textContent = s;
  return d.innerHTML;
}

function showToast(message, type, duration) {
  type = type || "info";
  duration = duration || 2800;
  var container = document.getElementById("toast-container");
  if (!container) return;
  var icons = { info: "\u2139\ufe0f", success: "\u2705", error: "\u274c", warning: "\u26a0\ufe0f" };
  var toast = document.createElement("div");
  toast.classList.add("toast");
  if (type !== "info") toast.classList.add("toast--" + type);
  toast.innerHTML = '<span class="toast__icon">' + (icons[type] || icons.info) + '</span><span>' + message + '</span>';
  container.appendChild(toast);
  setTimeout(function () { toast.remove(); }, duration);
}

// ============================================================
//  Relay Selector
// ============================================================

function _ctfPopulateRelaySelect() {
  var select = document.getElementById("ctf-relay-select");
  if (!select) return;
  var relays = typeof getRelays === "function" ? getRelays() : [];
  select.innerHTML = "";
  if (relays.length === 0) {
    var opt = document.createElement("option");
    opt.textContent = "No relays configured";
    opt.disabled = true;
    select.appendChild(opt);
    return;
  }
  relays.forEach(function (r) {
    var opt = document.createElement("option");
    opt.value = r.id;
    opt.textContent = r.name + " (" + r.ip + ")";
    select.appendChild(opt);
  });
}

function _ctfSelectedRelayId() {
  var select = document.getElementById("ctf-relay-select");
  return select ? select.value : null;
}

function _ctfSelectedRelayName() {
  var relayId = _ctfSelectedRelayId();
  var relays = typeof getRelays === "function" ? getRelays() : [];
  var relay = relays.find(function (r) { return r.id === relayId; });
  return relay ? relay.name : ("Relay " + relayId);
}

// ============================================================
//  WebSocket Connection
// ============================================================

function _ctfDisconnectRaw() {
  if (ctfWs) {
    ctfWs.onopen = null; ctfWs.onclose = null;
    ctfWs.onerror = null; ctfWs.onmessage = null;
    try { ctfWs.close(); } catch (_) {}
    ctfWs = null;
  }
  _ctfResolve = null;
  _ctfReject = null;
}

function _ctfEnsureConnection() {
  return new Promise(function (resolve, reject) {
    if (ctfWs && ctfWs.readyState === WebSocket.OPEN) return resolve(ctfWs);
    _ctfDisconnectRaw();
    _ctfSetStatus("connecting");
    var url = "ws://localhost:8765";
    ctfWs = new WebSocket(url);

    ctfWs.onopen = function () { resolve(ctfWs); };
    ctfWs.onerror = function () { reject(new Error("WebSocket connection failed")); };
    ctfWs.onclose = function () {
      if (_ctfReject) { _ctfReject(new Error("WebSocket closed")); _ctfResolve = null; _ctfReject = null; }
    };
    ctfWs.onmessage = function (event) {
      if (typeof event.data !== "string") return;
      if (_ctfResolve) { var r = _ctfResolve; _ctfResolve = null; _ctfReject = null; r(event.data); }
    };
  });
}

function _ctfSendCommand(cmd) {
  return new Promise(function (resolve, reject) {
    if (!ctfWs || ctfWs.readyState !== WebSocket.OPEN) {
      return reject(new Error("WebSocket not open"));
    }
    _ctfResolve = resolve;
    _ctfReject  = reject;
    var timer = setTimeout(function () {
      _ctfResolve = null; _ctfReject = null;
      reject(new Error("Timeout"));
    }, 30000);
    _ctfResolve = function (d) { clearTimeout(timer); resolve(d); };
    _ctfReject  = function (e) { clearTimeout(timer); reject(e); };

    var relayId = _ctfSelectedRelayId();
    var msg = relayId ? (relayId + ":" + cmd) : cmd;
    ctfWs.send(msg);
  });
}

// ============================================================
//  File Store & Rendering
// ============================================================

function _ctfAddFile(type, fileNum, text) {
  var entry = {
    id: _ctfNextId++,
    relay: _ctfSelectedRelayName(),
    relayId: _ctfSelectedRelayId(),
    type: type,
    fileNum: fileNum,
    text: text,
    fetchedAt: new Date().toLocaleTimeString()
  };
  _ctfFiles.unshift(entry);
  _ctfRenderFileList();
}

function _ctfDownloadEntry(entryId) {
  var entry = _ctfFiles.find(function (f) { return f.id === entryId; });
  if (!entry) return;
  var ext = entry.type === "cfg" ? ".cfg" : ".dat";
  var filename = "comtrade_" + entry.fileNum + ext;
  var blob = new Blob([entry.text], { type: "text/plain" });
  var url  = URL.createObjectURL(blob);
  var a    = document.createElement("a");
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

function _ctfRemoveEntry(entryId) {
  _ctfFiles = _ctfFiles.filter(function (f) { return f.id !== entryId; });
  _ctfRenderFileList();
}

function _ctfRenderFileList() {
  var container = document.getElementById("ctf-file-list");
  if (!container) return;

  if (_ctfFiles.length === 0) {
    container.innerHTML = '<div class="set-empty">No files fetched yet. Select a relay and file number above, then click Fetch.</div>';
    return;
  }

  var html = '';
  _ctfFiles.forEach(function (entry) {
    var ext = entry.type === "cfg" ? ".cfg" : ".dat";
    var label = entry.type === "cfg" ? "Configuration" : "Data";
    var icon = entry.type === "cfg" ? "\ud83d\udcc4" : "\ud83d\udcca";

    html += '<div class="ctf-file-card">';
    html += '  <div class="ctf-file-card__header">';
    html += '    <div class="ctf-file-card__info">';
    html += '      <span class="ctf-file-card__icon">' + icon + '</span>';
    html += '      <span class="ctf-file-card__name">comtrade_' + entry.fileNum + ext + '</span>';
    html += '      <span class="ctf-file-card__badge ctf-file-card__badge--' + entry.type + '">' + label + '</span>';
    html += '    </div>';
    html += '    <div class="ctf-file-card__meta">';
    html += '      <span class="ctf-file-card__relay">' + _ctfEscHtml(entry.relay) + '</span>';
    html += '      <span class="ctf-file-card__time">' + _ctfEscHtml(entry.fetchedAt) + '</span>';
    html += '    </div>';
    html += '  </div>';
    html += '  <div class="ctf-file-card__actions">';
    html += '    <button class="btn btn--primary btn--sm" onclick="_ctfDownloadEntry(' + entry.id + ')">\ud83d\udce5 Download ' + ext + '</button>';
    html += '    <button class="btn btn--outline btn--sm" onclick="_ctfTogglePreview(' + entry.id + ')">👁 Preview</button>';
    html += '    <button class="btn btn--danger btn--sm" onclick="_ctfRemoveEntry(' + entry.id + ')">✕</button>';
    html += '  </div>';
    html += '  <pre class="ctr-pre ctf-file-card__preview" id="ctf-preview-' + entry.id + '" style="display:none;">' + _ctfEscHtml(entry.text) + '</pre>';
    html += '</div>';
  });

  container.innerHTML = html;
}

function _ctfTogglePreview(entryId) {
  var el = document.getElementById("ctf-preview-" + entryId);
  if (!el) return;
  el.style.display = el.style.display === "none" ? "" : "none";
}

// ============================================================
//  Public Actions
// ============================================================

async function ctfFetchConfig() {
  var input = document.getElementById("ctf-file-num");
  var n = parseInt(input && input.value, 10);
  if (!n || n < 1) { showToast("Enter a valid file number", "error"); return; }
  if (!_ctfSelectedRelayId()) { showToast("Select a relay first", "error"); return; }

  _ctfSetBtns(true);
  try {
    await _ctfEnsureConnection();
    _ctfSetStatus("fetching");
    var cmd = "CTR C " + n;
    var response = await _ctfSendCommand(cmd);
    _ctfAddFile("cfg", n, response);
    _ctfSetStatus("done");
  } catch (err) {
    _ctfSetStatus("error");
    showToast("Config fetch failed: " + err.message, "error");
  } finally {
    _ctfSetBtns(false);
  }
}

async function ctfFetchData() {
  var input = document.getElementById("ctf-file-num");
  var n = parseInt(input && input.value, 10);
  if (!n || n < 1) { showToast("Enter a valid file number", "error"); return; }
  if (!_ctfSelectedRelayId()) { showToast("Select a relay first", "error"); return; }

  _ctfSetBtns(true);
  try {
    await _ctfEnsureConnection();
    _ctfSetStatus("fetching");
    var cmd = "CTR D " + n;
    var response = await _ctfSendCommand(cmd);
    _ctfAddFile("dat", n, response);
    _ctfSetStatus("done");
  } catch (err) {
    _ctfSetStatus("error");
    showToast("Data fetch failed: " + err.message, "error");
  } finally {
    _ctfSetBtns(false);
  }
}

async function ctfFetchBoth() {
  var input = document.getElementById("ctf-file-num");
  var n = parseInt(input && input.value, 10);
  if (!n || n < 1) { showToast("Enter a valid file number", "error"); return; }
  if (!_ctfSelectedRelayId()) { showToast("Select a relay first", "error"); return; }

  _ctfSetBtns(true);
  try {
    await _ctfEnsureConnection();
    _ctfSetStatus("fetching");

    var cfgResponse = await _ctfSendCommand("CTR C " + n);
    _ctfAddFile("cfg", n, cfgResponse);

    var datResponse = await _ctfSendCommand("CTR D " + n);
    _ctfAddFile("dat", n, datResponse);

    _ctfSetStatus("done");
    showToast("Both files fetched for #" + n, "success");
  } catch (err) {
    _ctfSetStatus("error");
    showToast("Fetch failed: " + err.message, "error");
  } finally {
    _ctfSetBtns(false);
  }
}

function ctfClearAll() {
  _ctfDisconnectRaw();
  _ctfFiles = [];
  _ctfNextId = 1;
  _ctfRenderFileList();
  _ctfSetStatus("idle");
  _ctfSetBtns(false);
}

// ============================================================
//  Bootstrap
// ============================================================
document.addEventListener("DOMContentLoaded", function () {
  _ctfPopulateRelaySelect();
});
