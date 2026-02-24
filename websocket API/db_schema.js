// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file db_schema.js
 * @brief Standalone helper for defining and inspecting SQLite table schemas via WebSocket.
 *
 * Self-contained — no dependency on db_client.js.  Provides:
 *   - define(table, columns) — create a table from a JSON column specification
 *   - schema(table)          — get column metadata (name, type, pk, notnull, …)
 *   - tables()               — list all user tables in the database
 *
 * @example
 * <script src="db_schema.js"></script>
 * <script>
 *   const s = new DBSchema('ws://localhost:8766');
 *   await s.connect();
 *
 *   // Define a table
 *   const { sql } = await s.define('config', [
 *     { name: 'id',    type: 'INTEGER', pk: true, autoincrement: true },
 *     { name: 'key',   type: 'TEXT',    notnull: true, unique: true },
 *     { name: 'value', type: 'TEXT',    default: "''" }
 *   ]);
 *   console.log('Created:', sql);
 *
 *   // Inspect
 *   const cols = await s.schema('config');
 *   console.table(cols);
 *
 *   // List tables
 *   const names = await s.tables();
 *   console.log(names); // ["config", "ser_records", ...]
 *
 *   s.close();
 * </script>
 *
 * @see ws_db_server.hpp   C++ server handling "define", "schema", and "tables" actions
 * @see db_client.js       Full-featured client (includes this + more)
 */

class DBSchema {
  /**
   * @param {string} url   WebSocket URL, e.g. "ws://localhost:8766"
   * @param {object} [opts]
   * @param {number} [opts.timeout=10000]  Per-request timeout (ms)
   */
  constructor(url, opts = {}) {
    this._url     = url;
    this._timeout = opts.timeout ?? 10000;
    this._ws      = null;
    this._nextId  = 1;
    this._pending = new Map();
  }

  // ── Connection ─────────────────────────────────────────────────────────

  /**
   * Open the WebSocket connection to the database server.
   *
   * @description If the socket is already open, resolves immediately.
   *              Sets up message routing for request/response matching.
   *
   * @returns {Promise<void>} Resolves when the WebSocket is open and ready.
   * @throws {Error} If WebSocket creation fails or connection is refused.
   */
  connect() {
    if (this._ws?.readyState === WebSocket.OPEN) return Promise.resolve();
    return new Promise((resolve, reject) => {
      try { this._ws = new WebSocket(this._url); } catch (e) { return reject(e); }
      this._ws.onopen    = () => resolve();
      this._ws.onclose   = () => { this._rejectAll('Connection closed'); };
      this._ws.onerror   = () => {};
      this._ws.onmessage = (ev) => this._onMsg(ev);
    });
  }

  /**
   * Close the WebSocket connection and reject any pending requests.
   *
   * @description All in-flight requests will be rejected with a "Closed" error.
   *              Safe to call multiple times.
   */
  close() {
    if (this._ws) { this._ws.close(); this._ws = null; }
    this._rejectAll('Closed');
  }

  /**
   * Check whether the WebSocket is currently connected.
   * @returns {boolean} True if the socket is open and ready.
   */
  get connected() { return this._ws?.readyState === WebSocket.OPEN; }

  // ── API ────────────────────────────────────────────────────────────────

  /**
   * Create a table from a JSON column definition.
   *
   * Each column object may contain:
   *   - name {string}           — column name (required)
   *   - type {string}           — SQL type, e.g. "TEXT", "INTEGER" (default: "TEXT")
   *   - pk {boolean}            — is primary key
   *   - autoincrement {boolean} — AUTOINCREMENT (only with pk: true)
   *   - notnull {boolean}       — NOT NULL constraint
   *   - unique {boolean}        — UNIQUE constraint
   *   - default {string}        — DEFAULT expression, e.g. "''"
   *
   * @param {string} table      Table name
   * @param {Object[]} columns  Column definitions array
   * @returns {Promise<{sql: string}>}  — the generated CREATE TABLE statement
   *
   * @example
   * await s.define('events', [
   *   { name: 'id',   type: 'INTEGER', pk: true, autoincrement: true },
   *   { name: 'ts',   type: 'TEXT',    notnull: true },
   *   { name: 'data', type: 'TEXT' }
   * ]);
   */
  async define(table, columns) {
    return this._send({ action: 'define', table, columns });
  }

  /**
   * Get column metadata for a table (PRAGMA table_info).
   *
   * @param {string} table  Table name
   * @returns {Promise<Array<{name:string, type:string, notnull:number, dflt_value:(string|null), pk:number}>>}
   */
  async schema(table) {
    const res = await this._send({ action: 'schema', table });
    return res.columns;
  }

  /**
   * List all user tables in the database.
   * @returns {Promise<string[]>}
   */
  async tables() {
    const res = await this._send({ action: 'tables' });
    return res.tables;
  }

  // ── Internals ──────────────────────────────────────────────────────────

  /**
   * Send a JSON payload over the WebSocket and return a Promise for the response.
   *
   * @description Assigns a unique request ID, sets a timeout timer, and stores
   *              the resolve/reject callbacks in the pending map. The matching
   *              response is routed by _onMsg() based on the ID.
   *
   * @private
   * @param {Object} payload  JSON-serialisable request object (action, table, etc.).
   * @returns {Promise<Object>} Resolves with the server's JSON response.
   * @throws {Error} If not connected or the request times out.
   */
  _send(payload) {
    return new Promise((resolve, reject) => {
      if (!this._ws || this._ws.readyState !== WebSocket.OPEN)
        return reject(new Error('WebSocket not connected'));
      const id = this._nextId++;
      payload.id = id;
      const timer = setTimeout(() => {
        this._pending.delete(id);
        reject(new Error(`Request ${id} timed out`));
      }, this._timeout);
      this._pending.set(id, { resolve, reject, timer });
      this._ws.send(JSON.stringify(payload));
    });
  }

  /**
   * Handle an incoming WebSocket message and route it to the matching pending request.
   *
   * @description Parses the JSON response, looks up the request ID in the
   *              pending map, and resolves or rejects the corresponding Promise.
   *
   * @private
   * @param {MessageEvent} ev  WebSocket message event.
   */
  _onMsg(ev) {
    let d; try { d = JSON.parse(ev.data); } catch { return; }
    const e = this._pending.get(d.id);
    if (!e) return;
    this._pending.delete(d.id);
    clearTimeout(e.timer);
    d.ok ? e.resolve(d) : e.reject(new Error(d.error || 'Server error'));
  }

  /**
   * Reject all pending requests with the given error message.
   *
   * @description Called on WebSocket close to clean up all in-flight requests.
   *
   * @private
   * @param {string} msg  Error message to pass to each rejected Promise.
   */
  _rejectAll(msg) {
    for (const [, e] of this._pending) { clearTimeout(e.timer); e.reject(new Error(msg)); }
    this._pending.clear();
  }
}

// UMD export
if (typeof module !== 'undefined' && module.exports) module.exports = { DBSchema };
