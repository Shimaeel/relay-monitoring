// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file db_query.js
 * @brief Standalone helper for running arbitrary SQL queries and writes via WebSocket.
 *
 * Self-contained — no dependency on db_client.js.  Provides:
 *   - query(sql, params)       — SELECT with bind parameters
 *   - exec(sql, params)        — INSERT / UPDATE / DELETE / DDL
 *   - queryObjects(sql, params) — SELECT returning array of {col: value} objects
 *   - count(table, where, params) — convenience row count
 *
 * @example
 * <script src="db_query.js"></script>
 * <script>
 *   const q = new DBQuery('ws://localhost:8766');
 *   await q.connect();
 *
 *   // SELECT
 *   const { columns, rows } = await q.query(
 *     'SELECT * FROM ser_records WHERE status = ? LIMIT ?',
 *     ['Asserted', '10']
 *   );
 *
 *   // INSERT
 *   const { changes, lastRowId } = await q.exec(
 *     'INSERT INTO config(key,value) VALUES(?,?)',
 *     ['theme', 'dark']
 *   );
 *
 *   // Objects helper
 *   const objs = await q.queryObjects('SELECT * FROM config');
 *   console.log(objs); // [{id:1, key:"theme", value:"dark"}, ...]
 *
 *   q.close();
 * </script>
 *
 * @see ws_db_server.hpp   C++ server handling "query" and "exec" actions
 * @see db_client.js       Full-featured client (includes this + more)
 */

class DBQuery {
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
   * Run an arbitrary SELECT query.
   *
   * @param {string}   sql       SQL SELECT (may contain ? placeholders)
   * @param {string[]} [params]  Bind parameters
   * @returns {Promise<{columns: string[], rows: any[][], rowCount: number, maxRowId: number}>}
   */
  async query(sql, params = []) {
    return this._send({ action: 'query', sql, params });
  }

  /**
   * Execute a write statement (INSERT / UPDATE / DELETE / DDL).
   *
   * @param {string}   sql       SQL statement (may contain ? placeholders)
   * @param {string[]} [params]  Bind parameters
   * @returns {Promise<{changes: number, lastRowId: number}>}
   */
  async exec(sql, params = []) {
    return this._send({ action: 'exec', sql, params });
  }

  /**
   * Query and return an array of plain {column: value} objects.
   *
   * @param {string}   sql
   * @param {string[]} [params]
   * @returns {Promise<Object[]>}
   *
   * @example
   * const users = await q.queryObjects('SELECT * FROM users WHERE active = ?', ['1']);
   * // [{id: 1, name: "Alice", active: "1"}, ...]
   */
  async queryObjects(sql, params = []) {
    const { columns, rows } = await this.query(sql, params);
    return rows.map(row => {
      const obj = {};
      columns.forEach((col, i) => { obj[col] = row[i]; });
      return obj;
    });
  }

  /**
   * Count rows in a table (with optional WHERE clause).
   *
   * @param {string}   table
   * @param {string}   [where]   e.g. "status = ?"
   * @param {string[]} [params]
   * @returns {Promise<number>}
   */
  async count(table, where = '', params = []) {
    let sql = `SELECT COUNT(*) AS cnt FROM ${table}`;
    if (where) sql += ` WHERE ${where}`;
    const { rows } = await this.query(sql, params);
    return parseInt(rows[0]?.[0] ?? '0', 10);
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
if (typeof module !== 'undefined' && module.exports) module.exports = { DBQuery };
