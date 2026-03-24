/**
 * ============================================================
 *  relay.js — Relay Detail Page Controller
 * ============================================================
 *
 *  Handles loading spinner, relay header, back navigation,
 *  sync time, error state, and toast notifications.
 *
 *  Dependencies: data.js must be loaded before this script.
 */

// ============================================================
//  DOM References
// ============================================================
const loadingOverlay    = document.getElementById("loading-overlay");
const relayHeaderName   = document.getElementById("relay-name");
const relaySubtitle     = document.getElementById("relay-subtitle");
const relayStatusBadge  = document.getElementById("relay-status-badge");
const syncTimeBtn       = document.getElementById("sync-time-btn");
const sntpSyncBtn       = document.getElementById("sntp-sync-btn");
const sntpConfig        = document.getElementById("sntp-config");
const sntpServerInput   = document.getElementById("sntp-server");
const sntpSendBtn       = document.getElementById("sntp-send-btn");
const sntpSyncStatusEl  = document.getElementById("sntp-sync-status");
const sntpOldTimeEl     = document.getElementById("sntp-old-time");
const sntpNewTimeEl     = document.getElementById("sntp-new-time");
const backBtn           = document.getElementById("back-btn");
const appContainer      = document.getElementById("app");
const relayImage        = document.getElementById("relay-image");
const toastContainer    = document.getElementById("toast-container");
const deviceTimeEl      = document.getElementById("device-time");
const pcTimeEl          = document.getElementById("pc-time");
const timeSyncStatusEl  = document.getElementById("time-sync-status");

// ============================================================
//  Initialisation
// ============================================================
document.addEventListener("DOMContentLoaded", () => {

  const relayId = getRelayIdFromURL();
  const relay   = getRelayById(relayId); // from data.js

  if (!relay) {
    showError(relayId);
    loadingOverlay.classList.add("hidden");
    return;
  }

  renderHeader(relay);
  bindEvents();
  initTimeSync(relay);

  // Dismiss loader
  setTimeout(() => loadingOverlay.classList.add("hidden"), 400);
});

// ============================================================
//  URL Helper
// ============================================================

function getRelayIdFromURL() {
  return new URLSearchParams(window.location.search).get("id");
}

// ============================================================
//  Rendering — Header
// ============================================================

let currentRelay = null;

function renderHeader(relay) {
  currentRelay = relay;
  relayHeaderName.textContent = relay.name;
  document.title = `${relay.name} — Relay Monitor`;
  relaySubtitle.textContent = `${relay.substation} · ${relay.bay}`;
  relayImage.src = `images/${relay.name}.svg`;
  relayImage.alt = `${relay.name} Relay Device`;

  const statusClass = relay.status === "online" ? "status-badge--online" : "status-badge--offline";
  const statusLabel = relay.status === "online" ? "Online" : "Offline";
  relayStatusBadge.className = `status-badge ${statusClass}`;
  relayStatusBadge.innerHTML = `<span class="status-badge__dot"></span> ${statusLabel}`;
}

// ============================================================
//  Event Bindings
// ============================================================

function bindEvents() {
  // Back
  backBtn.addEventListener("click", () => { window.location.href = "index.html"; });

  // Sync time — sends local PC time to relay via WebSocket
  syncTimeBtn.addEventListener("click", () => {
    sendTimeSyncAction("sync_time");
  });

  // SNTP Sync — toggle config panel
  sntpSyncBtn.addEventListener("click", () => {
    const isHidden = sntpConfig.style.display === "none";
    sntpConfig.style.display = isHidden ? "block" : "none";
  });

  // Send SNTP configuration to relay
  sntpSendBtn.addEventListener("click", () => {
    const server = sntpServerInput.value.trim();
    if (!server) {
      showToast("Enter an NTP server address", "warning");
      return;
    }
    sendSntpSyncAction(server);
  });
}

// ============================================================
//  Error State
// ============================================================

function showError(relayId) {
  const mc = document.querySelector(".main-content");
  mc.innerHTML = `
    <div class="error-container">
      <span class="error-container__icon">⚠️</span>
      <p class="error-container__msg">
        Relay ${relayId ? `#${relayId}` : ""} not found.
      </p>
      <a href="index.html" class="btn btn--primary">← Back to Dashboard</a>
    </div>
  `;
}

// ============================================================
//  Time Sync — WebSocket Communication
// ============================================================

let _timeSyncWs   = null;
let _timeSyncTimer = null;
let _deviceTimePoller = null;

/**
 * Initialise time sync: open WS, read relay time, start local clock.
 */
function initTimeSync(relay) {
  // Start local PC time ticker immediately
  updateLocalPCTime();
  _timeSyncTimer = setInterval(updateLocalPCTime, 1000);

  // Connect to the local WebSocket server (bridge to relay via Telnet)
  const wsUrl = `ws://localhost:${relay.wsPort}`;
  openTimeSyncWs(wsUrl);
}

function openTimeSyncWs(url) {
  if (_timeSyncWs && _timeSyncWs.readyState <= 1) return;

  _timeSyncWs = new WebSocket(url);

  _timeSyncWs.onopen = () => {
    updateRelayStatusBadge("online");
    // Read device time on connect, then poll every 10 s
    sendTimeSyncAction("read_time");
    if (_deviceTimePoller) clearInterval(_deviceTimePoller);
    _deviceTimePoller = setInterval(() => sendTimeSyncAction("read_time"), 10000);
  };

  _timeSyncWs.onmessage = (evt) => {
    // Only handle text (JSON) responses — ignore binary TLV
    if (typeof evt.data !== "string") return;
    try {
      const msg = JSON.parse(evt.data);
      handleTimeSyncResponse(msg);
    } catch (_) { /* not JSON, ignore */ }
  };

  _timeSyncWs.onclose = () => {
    updateRelayStatusBadge("offline");
    if (_deviceTimePoller) { clearInterval(_deviceTimePoller); _deviceTimePoller = null; }
    // Retry after 5 s in case the server is not yet up
    setTimeout(() => {
      if (currentRelay) openTimeSyncWs(`ws://localhost:${currentRelay.wsPort}`);
    }, 5000);
  };

  _timeSyncWs.onerror = () => {
    updateRelayStatusBadge("offline");
  };
}

function sendTimeSyncAction(action) {
  if (!_timeSyncWs || _timeSyncWs.readyState !== WebSocket.OPEN) {
    showToast("WebSocket not connected — cannot send " + action, "error");
    return;
  }
  _timeSyncWs.send(JSON.stringify({ action, relay_id: currentRelay.id }));

  if (action === "sync_time") {
    syncTimeBtn.disabled = true;
    syncTimeBtn.textContent = "⏳ Syncing…";
  }
}

function sendSntpSyncAction(server) {
  if (!_timeSyncWs || _timeSyncWs.readyState !== WebSocket.OPEN) {
    showToast("WebSocket not connected — cannot send SNTP config", "error");
    return;
  }
  _timeSyncWs.send(JSON.stringify({
    action: "sntp_sync",
    relay_id: currentRelay.id,
    sntp_server: server
  }));

  sntpSendBtn.disabled = true;
  sntpSendBtn.textContent = "⏳ Configuring…";
  sntpSyncStatusEl.textContent = "Sending SNTP configuration…";
}
function handleTimeSyncResponse(msg) {
  if (msg.action === "read_time") {
    if (msg.status === "success" && msg.relay_time) {
      deviceTimeEl.textContent = msg.relay_time;
    } else if (msg.status === "error") {
      deviceTimeEl.textContent = msg.error || "read failed";
    }
  }

  if (msg.action === "sync_time") {
    syncTimeBtn.disabled = false;
    syncTimeBtn.textContent = "⏱ Sync Time";

    if (msg.status === "success") {
      showToast("Relay time synced to " + msg.new_time, "success");
    } else {
      showToast("Sync failed: " + (msg.error || "unknown"), "error");
    }
  }

  if (msg.action === "sntp_sync") {
    sntpSendBtn.disabled = false;
    sntpSendBtn.textContent = "▶ Send SNTP Config";

    // Always show old device time if available
    if (msg.old_time) {
      sntpOldTimeEl.textContent = msg.old_time;
    }

    if (msg.status === "success") {
      sntpNewTimeEl.textContent = msg.new_time || "—";
      sntpSyncStatusEl.textContent = "✅ SNTP enabled → " + msg.sntp_server;
      showToast("SNTP configured: " + msg.sntp_server, "success");

      // Update the main device time display too
      if (msg.new_time) {
        deviceTimeEl.textContent = msg.new_time;
      }
    } else {
      sntpNewTimeEl.textContent = "—";
      const step = msg.step ? ` (step: ${msg.step})` : "";
      sntpSyncStatusEl.textContent = "❌ Failed" + step;
      showToast("SNTP config failed: " + (msg.error || "unknown") + step, "error");
    }
  }
}

function updateRelayStatusBadge(status) {
  const isOnline = status === "online";
  const statusClass = isOnline ? "status-badge--online" : "status-badge--offline";
  const statusLabel = isOnline ? "Online" : "Offline";
  relayStatusBadge.className = `status-badge ${statusClass}`;
  relayStatusBadge.innerHTML = `<span class="status-badge__dot"></span> ${statusLabel}`;
}

function updateLocalPCTime() {
  const now = new Date();
  const pad = (n) => String(n).padStart(2, "0");
  pcTimeEl.textContent =
    `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())} ` +
    `${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}`;
}

// ============================================================
//  Toast Notifications
// ============================================================

function showToast(message, type = "info", duration = 2800) {
  const icons = { info: "ℹ️", success: "✅", error: "❌", warning: "⚠️" };
  const toast = document.createElement("div");
  toast.classList.add("toast");
  if (type !== "info") toast.classList.add(`toast--${type}`);

  toast.innerHTML = `
    <span class="toast__icon">${icons[type] || icons.info}</span>
    <span>${message}</span>
  `;

  toastContainer.appendChild(toast);
  setTimeout(() => toast.remove(), duration);
}
