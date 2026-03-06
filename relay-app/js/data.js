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

const RELAYS = [
  {
    id: "1",
    name: "SEL-751",
    ip: "192.168.0.2",
    substation: "Substation Alpha",
    bay: "Bay 1",
    pse: "Transformer T1",
    breaker: "CB-101",
    status: "offline",
    wsPort: 8765
  },
  {
    id: "3",
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
