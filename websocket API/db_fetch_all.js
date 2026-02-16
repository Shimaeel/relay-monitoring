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

  /** Open the WebSocket. Resolves when ready. */
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

  /** Close the WebSocket. */
  close() {
    if (this._ws) { this._ws.close(); this._ws = null; }
    this._rejectAll('Closed');
  }

  /** @returns {boolean} */
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

  /** @private */
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

  /** @private */
  _onMsg(ev) {
    let d; try { d = JSON.parse(ev.data); } catch { return; }
    const e = this._pending.get(d.id);
    if (!e) return;
    this._pending.delete(d.id);
    clearTimeout(e.timer);
    d.ok ? e.resolve(d) : e.reject(new Error(d.error || 'Server error'));
  }

  /** @private */
  _rejectAll(msg) {
    for (const [, e] of this._pending) { clearTimeout(e.timer); e.reject(new Error(msg)); }
    this._pending.clear();
  }
}

// UMD export
if (typeof module !== 'undefined' && module.exports) module.exports = { DBFetchAll };
