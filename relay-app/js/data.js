/**
 * ============================================================
 *  data.js — Relay Data Store (Mock / Static)
 * ============================================================
 *
 *  Provides relay device data for the dashboard and detail pages.
 *  Replace with API calls or WebSocket feeds for production.
 *
 *  Dependencies: none
 */

// ============================================================
//  Relay Dataset
// ============================================================

const _DEFAULT_RELAYS = [
  {
    id: "1",
    name: "SEL-735",
    ip: "192.168.0.2",
    substation: "Substation Alpha",
    bay: "Bay 1",
    pse: "Transformer T1",
    breaker: "CB-101",
    status: "offline",
    wsPort: 8765
  },
  {
    id: "2",
    name: "SEL-451",
    ip: "192.168.0.3",
    substation: "Substation Alpha",
    bay: "Bay 3",
    pse: "Feeder F5",
    breaker: "CB-103",
    status: "offline",
    wsPort: 8765
  }
];

// Merge defaults with any user-added relays stored in localStorage
const RELAYS = (function () {
  const list = _DEFAULT_RELAYS.map(r => Object.assign({}, r));
  try {
    const saved = JSON.parse(localStorage.getItem("user_relays") || "[]");
    if (Array.isArray(saved)) {
      saved.forEach(r => {
        if (!list.find(d => d.id === r.id)) {
          list.push(r);
        }
      });
    }
  } catch (_) { /* ignore corrupt data */ }
  return list;
})();

// ============================================================
//  Public API
// ============================================================

/**
 * Get all relay devices.
 * @returns {Array} Full list of relay objects
 */
function getRelays() {
  return RELAYS;
}

/**
 * Get a single relay by its ID.
 * @param {string} id Relay identifier
 * @returns {Object|undefined} Relay object or undefined
 */
function getRelayById(id) {
  return RELAYS.find((r) => r.id === id);
}

/**
 * Add a new relay device to the dataset.
 * @param {Object} relay Relay object with name, ip, wsPort
 * @returns {Object} The newly added relay
 */
function addRelay(relay) {
  const maxId = RELAYS.reduce((max, r) => Math.max(max, parseInt(r.id, 10) || 0), 0);
  relay.id = String(maxId + 1);
  relay.status = "offline";
  RELAYS.push(relay);
  _saveUserRelays();
  return relay;
}

/**
 * Remove a relay by its ID.
 * @param {string} id Relay identifier
 * @returns {boolean} True if removed
 */
function removeRelay(id) {
  const idx = RELAYS.findIndex(r => r.id === id);
  if (idx === -1) return false;
  RELAYS.splice(idx, 1);
  _saveUserRelays();
  return true;
}

/**
 * Persist user-added relays (those not in defaults) to localStorage.
 */
function _saveUserRelays() {
  const defaultIds = new Set(_DEFAULT_RELAYS.map(r => r.id));
  const userRelays = RELAYS.filter(r => !defaultIds.has(r.id));
  try {
    localStorage.setItem("user_relays", JSON.stringify(userRelays));
  } catch (_) { /* storage full or unavailable */ }
}
