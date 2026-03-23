/**
 * ============================================================
 *  dashboard.js — Dashboard Controller
 * ============================================================
 *
 *  Handles loading spinner, relay card grid,
 *  search/filter, stats, clock, and toast notifications.
 *
 *  Dependencies: data.js must be loaded before this script.
 */

// ============================================================
//  DOM References
// ============================================================
const loadingOverlay = document.getElementById("loading-overlay");
const relayGrid      = document.getElementById("relay-grid");
const searchInput    = document.getElementById("search-input");
const refreshBtn     = document.getElementById("refresh-btn");
const toastContainer = document.getElementById("toast-container");

// ============================================================
//  State
// ============================================================
let allRelays = [];

// ============================================================
//  Initialisation
// ============================================================
document.addEventListener("DOMContentLoaded", () => {
  loadRelays();
  bindEvents();

  // Dismiss loading spinner after short delay
  setTimeout(() => {
    loadingOverlay.classList.add("hidden");
  }, 500);
});

// ============================================================
//  Data Loading
// ============================================================

function loadRelays() {
  allRelays = getRelays(); // from data.js
  renderCards(allRelays);
  probeRelayStatus(allRelays);
}

// ============================================================
//  Rendering — Relay Cards
// ============================================================

function renderCards(relayList) {
  relayGrid.innerHTML = "";

  if (relayList.length === 0) {
    relayGrid.innerHTML = `
      <div class="empty-state">
        <div class="empty-state__icon">🔍</div>
        <div class="empty-state__title">No relays found</div>
        <p class="empty-state__desc">No devices match your current search criteria. Try adjusting your filters.</p>
      </div>
    `;
    return;
  }

  relayList.forEach((relay) => {
    relayGrid.appendChild(createCardElement(relay));
  });
}

function createCardElement(relay) {
  const card = document.createElement("article");
  card.classList.add("relay-card");
  card.setAttribute("role", "button");
  card.setAttribute("tabindex", "0");
  card.dataset.id = relay.id;

  const statusClass = relay.status === "online" ? "status-badge--online" : "status-badge--offline";
  const statusLabel = relay.status === "online" ? "Online" : "Offline";

  card.innerHTML = `
    <button class="relay-card__remove" title="Remove relay" aria-label="Remove ${relay.name}">✕</button>
    <div class="relay-card__body">
      <div class="relay-card__img-wrap">
        <img class="relay-card__img" src="images/${relay.name}.svg" alt="${relay.name}" onerror="this.src='images/SEL-relay.svg'" />
      </div>
      <div class="relay-card__info">
        <span class="relay-card__name">${relay.name}</span>
        <span class="relay-card__ip">${relay.ip}</span>
        <span class="relay-card__meta">${relay.substation} · ${relay.bay}</span>
      </div>
    </div>
    <div class="relay-card__footer">
      <span class="status-badge ${statusClass}">
        <span class="status-badge__dot"></span>
        ${statusLabel}
      </span>
      <span class="relay-card__arrow">→</span>
    </div>
  `;

  // Remove button
  card.querySelector(".relay-card__remove").addEventListener("click", (e) => {
    e.stopPropagation();
    handleRemoveRelay(relay);
  });

  card.addEventListener("click", () => navigateToRelay(relay.id));
  card.addEventListener("keydown", (e) => {
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      navigateToRelay(relay.id);
    }
  });

  return card;
}

// ============================================================
//  Live Status Probing — WebSocket ping each relay
// ============================================================

const _probeWsMap = {};   // relayId → WebSocket
const _probeTimers = {};  // relayId → retry timer

function probeRelayStatus(relayList) {
  // Close any existing probes first
  Object.values(_probeWsMap).forEach(ws => { try { ws.close(); } catch (_) {} });

  relayList.forEach(relay => probeOneRelay(relay));
}

function probeOneRelay(relay) {
  if (_probeWsMap[relay.id] && _probeWsMap[relay.id].readyState <= 1) return;

  const ws = new WebSocket(`ws://localhost:${relay.wsPort}`);
  _probeWsMap[relay.id] = ws;

  ws.onopen = () => {
    updateCardStatus(relay.id, "online");
    relay.status = "online";
  };

  ws.onclose = () => {
    updateCardStatus(relay.id, "offline");
    relay.status = "offline";
    // Re-probe after 10s
    clearTimeout(_probeTimers[relay.id]);
    _probeTimers[relay.id] = setTimeout(() => probeOneRelay(relay), 10000);
  };

  ws.onerror = () => {
    updateCardStatus(relay.id, "offline");
    relay.status = "offline";
  };
}

function updateCardStatus(relayId, status) {
  const card = relayGrid.querySelector(`[data-id="${relayId}"]`);
  if (!card) return;

  const badge = card.querySelector(".status-badge");
  if (!badge) return;

  const isOnline = status === "online";
  badge.className = `status-badge ${isOnline ? "status-badge--online" : "status-badge--offline"}`;
  badge.innerHTML = `<span class="status-badge__dot"></span> ${isOnline ? "Online" : "Offline"}`;
}

// ============================================================
//  Event Handlers
// ============================================================

function bindEvents() {
  // Search
  searchInput.addEventListener("input", handleSearch);

  // Refresh
  refreshBtn.addEventListener("click", handleRefresh);
}

function handleSearch() {
  const query = searchInput.value.trim().toLowerCase();
  const filtered = allRelays.filter((r) =>
    r.name.toLowerCase().includes(query) ||
    r.ip.toLowerCase().includes(query) ||
    r.substation.toLowerCase().includes(query)
  );
  renderCards(filtered);
}

function handleRefresh() {
  refreshBtn.disabled = true;
  refreshBtn.textContent = "Refreshing…";

  setTimeout(() => {
    loadRelays();
    if (searchInput.value.trim()) handleSearch();
    refreshBtn.disabled = false;
    refreshBtn.textContent = "⟳ Refresh";
    showToast("Dashboard refreshed — probing relay status…", "success");
  }, 600);
}

// ============================================================
//  Navigation
// ============================================================

function navigateToRelay(id) {
  window.location.href = `relay.html?id=${id}`;
}

// ============================================================
//  Toast Notifications
// ============================================================

/**
 * Show a toast notification.
 * @param {string} message
 * @param {"info"|"success"|"error"|"warning"} type
 * @param {number} duration - ms
 */
// ============================================================
//  Add Relay Form
// ============================================================

const addRelayForm = document.getElementById("add-relay-form");

if (addRelayForm) {
  addRelayForm.addEventListener("submit", (e) => {
    e.preventDefault();

    const name = document.getElementById("relay-name-input").value.trim();
    const ip   = document.getElementById("relay-ip-input").value.trim();
    const port = parseInt(document.getElementById("relay-port-input").value, 10);

    if (!name || !ip || !port) {
      showToast("Please fill in all fields.", "warning");
      return;
    }

    const newRelay = addRelay({
      name,
      ip,
      substation: "",
      bay: "",
      pse: "",
      breaker: "",
      wsPort: port
    });

    // Re-render cards
    allRelays = getRelays();
    renderCards(allRelays);
    probeOneRelay(newRelay);

    showToast(`Relay "${name}" added successfully.`, "success");
    addRelayForm.reset();
  });
}

// ============================================================
//  Remove Relay — Confirmation Modal
// ============================================================

const _confirmModal   = document.getElementById("confirm-modal");
const _confirmName    = document.getElementById("confirm-modal-name");
const _confirmIp      = document.getElementById("confirm-modal-ip");
const _confirmCancel  = document.getElementById("confirm-modal-cancel");
const _confirmOk      = document.getElementById("confirm-modal-confirm");
const _confirmBackdrop = _confirmModal ? _confirmModal.querySelector(".confirm-modal__backdrop") : null;
let _pendingRemoveRelay = null;

function handleRemoveRelay(relay) {
  if (!_confirmModal) return;
  _pendingRemoveRelay = relay;
  _confirmName.textContent = relay.name;
  _confirmIp.textContent   = relay.ip;
  _confirmModal.classList.add("visible");
  _confirmModal.setAttribute("aria-hidden", "false");
  _confirmOk.focus();
}

function _closeConfirmModal() {
  _confirmModal.classList.remove("visible");
  _confirmModal.setAttribute("aria-hidden", "true");
  _pendingRemoveRelay = null;
}

function _executeRemoveRelay() {
  const relay = _pendingRemoveRelay;
  if (!relay) return;

  removeRelay(relay.id);

  if (_probeWsMap[relay.id]) {
    try { _probeWsMap[relay.id].close(); } catch (_) {}
    delete _probeWsMap[relay.id];
  }
  clearTimeout(_probeTimers[relay.id]);
  delete _probeTimers[relay.id];

  allRelays = getRelays();
  renderCards(allRelays);
  showToast(`Relay "${relay.name}" removed.`, "success");
  _closeConfirmModal();
}

if (_confirmOk)      _confirmOk.addEventListener("click", _executeRemoveRelay);
if (_confirmCancel)  _confirmCancel.addEventListener("click", _closeConfirmModal);
if (_confirmBackdrop) _confirmBackdrop.addEventListener("click", _closeConfirmModal);
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape" && _confirmModal && _confirmModal.classList.contains("visible")) {
    _closeConfirmModal();
  }
});

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
