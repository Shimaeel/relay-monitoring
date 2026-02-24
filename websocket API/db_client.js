// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file db_client.js
 * @brief Generic WebSocket client for SQLite database access.
 *
 * Connects to the WSDBServer (ws_db_server.hpp) and provides a
 * Promise-based API for:
 *
 *  - **getAll(table)**                    – every row in a table
 *  - **getIncremental(table, sinceRowId)** – only new rows since a rowid
 *  - **query(sql, params)**               – arbitrary SELECT
 *  - **exec(sql, params)**                – INSERT / UPDATE / DELETE / DDL
 *  - **tables()**                         – list all user tables
 *  - **schema(table)**                    – column metadata
 *  - **define(table, columns)**           – create table from JSON definition
 *
 * ## Quick Start
 *
 * ```html
 * <script src="db_client.js"></script>
 * <script>
 *   const db = new DatabaseClient('ws://localhost:8766');
 *   await db.connect();
 *
 *   // Define a table via JSON
 *   await db.define('config', [
 *     { name: 'id',    type: 'INTEGER', pk: true, autoincrement: true },
 *     { name: 'key',   type: 'TEXT',    notnull: true, unique: true },
 *     { name: 'value', type: 'TEXT',    default: "''" }
 *   ]);
 *
 *   // Get everything
 *   const all = await db.getAll('ser_records');          // {columns, rows, maxRowId}
 *
 *   // Get incremental updates
 *   const inc = await db.getIncremental('ser_records', all.maxRowId);
 *
 *   // Arbitrary SELECT
 *   const res = await db.query('SELECT * FROM ser_records WHERE status=?', ['Asserted']);
 *
 *   db.close();
 * </script>
 * ```
 *
 * @see ws_db_server.hpp  C++ WebSocket server that handles these requests
 */

class DatabaseClient {
  /**
   * @param {string} url  WebSocket URL, e.g. "ws://localhost:8766"
   * @param {object} [opts]
   * @param {number}  [opts.timeout=10000]       Per-request timeout (ms)
   * @param {boolean} [opts.autoReconnect=true]
   * @param {number}  [opts.reconnectDelay=3000]
   */
  constructor(url, opts = {}) {
    this._url            = url;
    this._timeout        = opts.timeout ?? 10000;
    this._autoReconnect  = opts.autoReconnect ?? true;
    this._reconnectDelay = opts.reconnectDelay ?? 3000;

    /** @type {WebSocket|null} */
    this._ws             = null;
    this._nextId         = 1;
    /** @type {Map<number, {resolve, reject, timer}>} */
    this._pending        = new Map();
    this._connected      = false;
    this._connectPromise = null;
    this._listeners      = { open: [], close: [], error: [] };
  }

  // ─── Connection management ─────────────────────────────────────────────

  /**
   * Open the WebSocket connection.  Resolves when the socket is open.
   * @returns {Promise<void>}
   */
  connect() {
    if (this._connected && this._ws?.readyState === WebSocket.OPEN)
      return Promise.resolve();
    if (this._connectPromise) return this._connectPromise;

    this._connectPromise = new Promise((resolve, reject) => {
      try { this._ws = new WebSocket(this._url); }
      catch (e) { this._connectPromise = null; return reject(e); }

      this._ws.onopen = () => {
        this._connected = true;
        this._connectPromise = null;
        this._emit('open');
        resolve();
      };
      this._ws.onclose  = (ev) => { this._handleClose(ev); if (!this._connected) { this._connectPromise = null; reject(new Error('Closed before open')); } };
      this._ws.onerror  = (ev) => { this._emit('error', ev); };
      this._ws.onmessage = (ev) => this._onMessage(ev);
    });
    return this._connectPromise;
  }

  /** Close the connection (disables auto-reconnect). */
  close() {
    this._autoReconnect = false;
    if (this._ws) { this._ws.close(); this._ws = null; }
    this._connected = false;
    for (const [, entry] of this._pending) { clearTimeout(entry.timer); entry.reject(new Error('Connection closed')); }
    this._pending.clear();
  }

  /** @returns {boolean} */
  get connected() { return this._connected; }

  // ─── Core API ──────────────────────────────────────────────────────────

  /**
   * Fetch every row in a table.
   *
   * @param {string} table  Table name
   * @returns {Promise<{columns:string[], rows:any[][], rowCount:number, maxRowId:number}>}
   *
   * @example
   * const { columns, rows, maxRowId } = await db.getAll('ser_records');
   */
  async getAll(table) {
    return this._send({ action: 'getAll', table });
  }

  /**
   * Fetch rows added since a given rowid (incremental update).
   *
   * @param {string} table       Table name
   * @param {number} sinceRowId  Only return rows with rowid > this value (use 0 for first call)
   * @returns {Promise<{columns:string[], rows:any[][], rowCount:number, maxRowId:number}>}
   *
   * @example
   * // First call – get everything
   * let { maxRowId } = await db.getAll('ser_records');
   * // Later – get only new rows
   * const update = await db.getIncremental('ser_records', maxRowId);
   * maxRowId = update.maxRowId || maxRowId;
   */
  async getIncremental(table, sinceRowId = 0) {
    return this._send({ action: 'getIncremental', table, sinceRowId });
  }

  /**
   * Run an arbitrary SELECT query.
   *
   * @param {string}   sql        SQL SELECT (may contain ? placeholders)
   * @param {string[]} [params]   Bind parameters
   * @returns {Promise<{columns:string[], rows:any[][], rowCount:number, maxRowId:number}>}
   *
   * @example
   * const res = await db.query(
   *   'SELECT * FROM ser_records WHERE status = ? ORDER BY timestamp DESC LIMIT ?',
   *   ['Asserted', '10']
   * );
   */
  async query(sql, params = []) {
    return this._send({ action: 'query', sql, params });
  }

  /**
   * Execute a write statement (INSERT / UPDATE / DELETE / DDL).
   *
   * @param {string}   sql       SQL statement (may contain ? placeholders)
   * @param {string[]} [params]  Bind parameters
   * @returns {Promise<{changes:number, lastRowId:number}>}
   *
   * @example
   * await db.exec(
   *   'INSERT INTO ser_records(record_id,timestamp,status,description) VALUES(?,?,?,?)',
   *   ['99', '2026-01-01 00:00:00', 'Asserted', 'Manual entry']
   * );
   */
  async exec(sql, params = []) {
    return this._send({ action: 'exec', sql, params });
  }

  /**
   * List all user tables in the database.
   * @returns {Promise<string[]>}
   */
  async tables() {
    const res = await this._send({ action: 'tables' });
    return res.tables;
  }

  /**
   * Get column metadata for a table.
   *
   * @param {string} table  Table name
   * @returns {Promise<Array<{name:string, type:string, notnull:number, dflt_value:(string|null), pk:number}>>}
   */
  async schema(table) {
    const res = await this._send({ action: 'schema', table });
    return res.columns;
  }

  /**
   * Create a table from a JSON column definition.
   *
   * @param {string} table     Table name
   * @param {Array<Object>} columns  Column definitions (name, type, pk, autoincrement, notnull, unique, default)
   * @returns {Promise<{sql:string}>}  — the generated CREATE TABLE statement
   *
   * @example
   * await db.define('config', [
   *   { name: 'id',    type: 'INTEGER', pk: true, autoincrement: true },
   *   { name: 'key',   type: 'TEXT',    notnull: true, unique: true },
   *   { name: 'value', type: 'TEXT',    default: "''" }
   * ]);
   */
  async define(table, columns) {
    return this._send({ action: 'define', table, columns });
  }

  // ─── Convenience helpers ───────────────────────────────────────────────

  /**
   * Query and return an array of plain objects (column → value).
   *
   * @param {string}   sql
   * @param {string[]} [params]
   * @returns {Promise<Object[]>}
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

  /**
   * Start polling for incremental updates at a fixed interval.
   *
   * @param {string}   table
   * @param {function} onRows      Called with ({columns, rows, maxRowId}) for each batch
   * @param {object}   [opts]
   * @param {number}   [opts.intervalMs=2000]
   * @param {number}   [opts.sinceRowId=0]
   * @returns {Object}  Object with a stop() method to cancel polling
   *
   * @example
   * const poll = db.poll('ser_records', ({ rows, maxRowId }) => {
   *   console.log(`Got ${rows.length} new rows up to rowid ${maxRowId}`);
   * }, { intervalMs: 3000 });
   *
   * // Later...
   * poll.stop();
   */
  poll(table, onRows, opts = {}) {
    let sinceRowId = opts.sinceRowId ?? 0;
    const ms       = opts.intervalMs ?? 2000;
    let stopped    = false;

    const tick = async () => {
      if (stopped || !this._connected) return;
      try {
        const res = await this.getIncremental(table, sinceRowId);
        if (res.rows.length > 0) {
          sinceRowId = res.maxRowId;
          onRows(res);
        }
      } catch (e) {
        console.error('[DatabaseClient] poll error:', e);
      }
      if (!stopped) timer = setTimeout(tick, ms);
    };

    let timer = setTimeout(tick, 0); // first call immediately
    return {
      stop() { stopped = true; clearTimeout(timer); },
      get sinceRowId() { return sinceRowId; }
    };
  }

  // ─── Event system ──────────────────────────────────────────────────────

  /**
   * Register a listener for a connection lifecycle event.
   *
   * @description Supported events:
   * - `'open'`  — Fired when the WebSocket connection is established.
   * - `'close'` — Fired when the connection drops (with the CloseEvent).
   * - `'error'` — Fired on WebSocket error (with the error Event).
   *
   * @param {'open'|'close'|'error'} event  Event name to listen for.
   * @param {Function} fn  Callback function invoked when the event fires.
   */
  on(event, fn)  { (this._listeners[event] ??= []).push(fn); }

  /**
   * Remove a previously registered event listener.
   *
   * @param {'open'|'close'|'error'} event  Event name.
   * @param {Function} fn  The exact function reference passed to on().
   */
  off(event, fn) { const a = this._listeners[event]; if (a) this._listeners[event] = a.filter(f => f !== fn); }

  // ─── Internals ─────────────────────────────────────────────────────────

  /**
   * Emit an event to all registered listeners.
   *
   * @private
   * @param {'open'|'close'|'error'} event  Event name to emit.
   * @param {...*} args  Arguments to pass to listener callbacks.
   */
  _emit(event, ...args) {
    for (const fn of (this._listeners[event] ?? []))
      try { fn(...args); } catch (e) { console.error('[DatabaseClient]', e); }
  }

  /**
   * Send a JSON payload over the WebSocket and return a Promise for the response.
   *
   * @description Assigns a unique request ID, sets a timeout timer, and stores
   *              the resolve/reject callbacks in the pending map. The matching
   *              response is routed by _onMessage() based on the ID.
   *
   * @private
   * @param {Object} payload  JSON-serialisable request object (action, table, sql, etc.).
   * @returns {Promise<Object>} Resolves with the server's JSON response.
   * @throws {Error} If not connected or the request times out.
   */
  _send(payload) {
    return new Promise((resolve, reject) => {
      if (!this._ws || this._ws.readyState !== WebSocket.OPEN)
        return reject(new Error('WebSocket not connected'));

      const id  = this._nextId++;
      payload.id = id;

      const timer = setTimeout(() => {
        this._pending.delete(id);
        reject(new Error(`Request ${id} timed out after ${this._timeout}ms`));
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
   *              Invalid JSON messages are logged and ignored.
   *
   * @private
   * @param {MessageEvent} ev  WebSocket message event containing a JSON response.
   */
  _onMessage(ev) {
    let data;
    try { data = JSON.parse(ev.data); }
    catch (e) { console.error('[DatabaseClient] Bad JSON:', ev.data); return; }

    const entry = this._pending.get(data.id);
    if (!entry) return;
    this._pending.delete(data.id);
    clearTimeout(entry.timer);

    if (data.ok) entry.resolve(data);
    else         entry.reject(new Error(data.error || 'Server error'));
  }

  /**
   * Handle WebSocket close: clean up state, reject pending requests, and
   * optionally schedule an automatic reconnection attempt.
   *
   * @description Emits a 'close' event, rejects all in-flight requests
   *              with "Connection lost", and schedules reconnect if
   *              autoReconnect is enabled.
   *
   * @private
   * @param {CloseEvent} ev  The WebSocket CloseEvent.
   */
  _handleClose(ev) {
    const was = this._connected;
    this._connected = false;
    this._ws = null;
    this._emit('close', ev);
    for (const [, e] of this._pending) { clearTimeout(e.timer); e.reject(new Error('Connection lost')); }
    this._pending.clear();
    if (was && this._autoReconnect)
      setTimeout(() => { if (this._autoReconnect) this.connect().catch(() => {}); }, this._reconnectDelay);
  }
}

// UMD export
if (typeof module !== 'undefined' && module.exports) module.exports = { DatabaseClient };
