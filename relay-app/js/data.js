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
    status: "online",
    wsPort: 8765
  },
  {
    id: "2",
    name: "SEL-421",
    ip: "192.168.1.11",
    substation: "Substation Alpha",
    bay: "Bay 2",
    pse: "Feeder F3",
    breaker: "CB-102",
    status: "online",
    wsPort: 8766
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
