// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file db_fetch_all.js
 * @brief Standalone helper for fetching every row in a SQLite table via WebSocket.
 *
 * Self-contained — no dependency on db_client.js.  Include this single file
 * to add full-table fetch to any page.
 *
 * @example
 * <script src="db_fetch_all.js"></script>
 * <script>
 *   const fetcher = new DBFetchAll('ws://localhost:8766');
 *   await fetcher.connect();
 *   const { columns, rows, maxRowId } = await fetcher.getAll('ser_records');
 *   console.table(rows);
 *   fetcher.close();
 * </script>
 *
 * @see ws_db_server.hpp   C++ server that handles the "getAll" action
 * @see db_client.js       Full-featured client (includes this + more)
 */

class DBFetchAll {
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
   *              Configures message routing for request/response matching.
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
   * Fetch every row in a table.
   *
   * @param {string} table  Table name
   * @returns {Promise<{columns: string[], rows: any[][], rowCount: number, maxRowId: number}>}
   *
   * @example
   * const { columns, rows, maxRowId } = await fetcher.getAll('ser_records');
   */
  async getAll(table) {
    return this._send({ action: 'getAll', table });
  }

  // ── Internals ──────────────────────────────────────────────────────────

  /**
   * Send a JSON payload over the WebSocket and return a Promise for the response.
   *
   * @description Assigns a unique request ID, sets a timeout timer, and stores
   *              the resolve/reject callbacks in the pending map.
   *
   * @private
   * @param {Object} payload  JSON-serialisable request object.
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
   * @private
   * @param {MessageEvent} ev  WebSocket message event containing a JSON response.
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
if (typeof module !== 'undefined' && module.exports) module.exports = { DBFetchAll };
