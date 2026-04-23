/**
 * ============================================================
 *  relay.js — Relay Detail Page Controller
 * ============================================================
 *
 *  Handles loading spinner, relay header, back navigation,
 *  error state, and toast notifications.
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
const backBtn           = document.getElementById("back-btn");
const appContainer      = document.getElementById("app");
const relayImage        = document.getElementById("relay-image");
const toastContainer    = document.getElementById("toast-container");

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
  probeRelayStatus(relay);

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
//  Relay Status Badge
// ============================================================

function updateRelayStatusBadge(status) {
  const isOnline = status === "online";
  const statusClass = isOnline ? "status-badge--online" : "status-badge--offline";
  const statusLabel = isOnline ? "Online" : "Offline";
  relayStatusBadge.className = `status-badge ${statusClass}`;
  relayStatusBadge.innerHTML = `<span class="status-badge__dot"></span> ${statusLabel}`;
}

// ============================================================
//  Live Status Probing — WebSocket keepalive
// ============================================================

let _probeWs = null;
let _probeRetryTimer = null;

function probeRelayStatus(relay) {
  if (_probeWs && _probeWs.readyState <= 1) return;

  const ws = new WebSocket(`ws://localhost:${relay.wsPort}`);
  _probeWs = ws;

  ws.onopen = () => {
    relay.status = "online";
    updateRelayStatusBadge("online");
  };

  ws.onclose = () => {
    relay.status = "offline";
    updateRelayStatusBadge("offline");
    clearTimeout(_probeRetryTimer);
    _probeRetryTimer = setTimeout(() => probeRelayStatus(relay), 10000);
  };

  ws.onerror = () => {
    relay.status = "offline";
    updateRelayStatusBadge("offline");
  };
}

window.addEventListener("beforeunload", () => {
  clearTimeout(_probeRetryTimer);
  if (_probeWs) { try { _probeWs.close(); } catch (_) {} }
});

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
