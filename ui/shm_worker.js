// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file shm_worker.js
 * @brief Web Worker that bridges WebSocket input to a SharedArrayBuffer MPMC ring buffer.
 *
 * @description This Web Worker runs in a background thread and performs two jobs:
 *
 * 1. **WebSocket consumer** — Connects to the SER WebSocket server, receives
 *    JSON arrays of SER records, and encodes them into BER-TLV binary payloads.
 *
 * 2. **Ring buffer producer** — Writes the encoded payloads into a
 *    SharedArrayBuffer-backed MPMC ring buffer so multiple readers (main thread,
 *    other workers) can each consume every message independently.
 *
 * ## Communication Protocol (main thread → worker)
 *
 * | Message type | Fields                     | Description                             |
 * |-------------|----------------------------|-----------------------------------------|
 * | `init`      | `buffer`, `capacity`       | Provide the SharedArrayBuffer mapping.  |
 * | `connect`   | `wsUrl`                    | Connect to the SER WebSocket server.    |
 * | `send`      | `payload`                  | Send a raw command over the WebSocket.  |
 *
 * ## Communication Protocol (worker → main thread)
 *
 * | Message type  | Fields              | Description                          |
 * |--------------|---------------------|--------------------------------------|
 * | `ws_status`  | `status`            | WebSocket state: connecting/connected/disconnected/error |
 * | `error`      | `message`           | Error details (JSON parse fail, etc.)|
 *
 * ## MPMC Ring Buffer Memory Layout
 *
 * ```
 * SharedArrayBuffer:
 * ┌────────────────────────────────────────────────────────────────────────────┐
 * │ Int32[0]: writePos                                                        │
 * │ Int32[1]: signal (notification counter)                                   │
 * │ Int32[2]: readerCount                                                     │
 * │ Int32[3..10]: per-reader readPos (-1 = inactive)                          │
 * ├──────────────────────────────────────────────────────────────────────────────┤
 * │ Uint8[headerBytes .. headerBytes + capacity]: data ring                    │
 * │   [4-byte LE len][payload bytes][4-byte LE len][payload]...               │
 * └──────────────────────────────────────────────────────────────────────────────┘
 * ```
 *
 * @see shared_ring_buffer.hpp  C++ equivalent ring buffer implementation
 * @see asn_tlv_codec.hpp       C++ BER-TLV codec matching the JS encoding here
 * @see index.html              Main thread consumer that reads the ring buffer
 *
 * @author Telnet-SML Development Team
 * @version 2.0.0
 * @date 2026
 */

/**
 * Shared MPMC ring buffer state used by the worker.
 * @type {{header: Int32Array|null, data: Uint8Array|null, view: DataView|null, capacity: number, maxReaders: number}}
 */
const STATE = {
    header: null,
    data: null,
    view: null,
    capacity: 0,
    maxReaders: 8
};

/** @type {WebSocket|null} */
let ws = null;
/** @type {string|null} */
let wsUrl = null;

/**
 * Initialize the ring buffer views for the shared buffer.
 * Header layout: [writePos, signal, readerCount, readPos0..readPos7]
 * @param {ArrayBuffer|SharedArrayBuffer} buffer
 * @param {number} capacity
 */
function initRing(buffer, capacity) {
    const headerSlots = 3 + STATE.maxReaders;
    const headerBytes = headerSlots * 4;
    STATE.header = new Int32Array(buffer, 0, headerSlots);
    STATE.data = new Uint8Array(buffer, headerBytes, capacity);
    STATE.view = new DataView(buffer, headerBytes, capacity);
    STATE.capacity = capacity;
}

/**
 * Compute free bytes available in the ring buffer.
 * Uses the slowest active reader for backpressure.
 * @param {number} writePos
 * @returns {number}
 */
function freeBytes(writePos) {
    const slowReadPos = slowestReaderPos(writePos);
    if (writePos >= slowReadPos) {
        return STATE.capacity - (writePos - slowReadPos);
    }
    return slowReadPos - writePos;
}

/**
 * Find the slowest (most behind) active reader position.
 * If no readers are active, returns writePos (all space is free).
 * @param {number} writePos
 * @returns {number}
 */
function slowestReaderPos(writePos) {
    let slowest = writePos;
    let maxUsed = 0;
    let anyActive = false;

    for (let i = 0; i < STATE.maxReaders; i++) {
        const rp = Atomics.load(STATE.header, 3 + i);
        if (rp < 0) continue;  // inactive reader
        anyActive = true;
        const used = writePos >= rp
            ? writePos - rp
            : STATE.capacity - (rp - writePos);
        if (used > maxUsed) {
            maxUsed = used;
            slowest = rp;
        }
    }

    return anyActive ? slowest : writePos;
}

/**
 * Read a 32-bit little-endian length from the ring buffer.
 * @param {number} pos
 * @returns {number}
 */
function readLength(pos) {
    return STATE.view.getUint32(pos, true);
}

/**
 * Write a 32-bit little-endian length into the ring buffer.
 * @param {number} pos
 * @param {number} len
 */
function writeLength(pos, len) {
    STATE.view.setUint32(pos, len, true);
}

/**
 * Drop the oldest record for the slowest reader(s) to free space.
 * Advances ALL readers at the slowest position past one message.
 * @param {number} writePos
 * @returns {void}
 */
function dropOldestForSlowest(writePos) {
    // Find the slowest reader position
    let slowestPos = writePos;
    let maxUsed = 0;

    for (let i = 0; i < STATE.maxReaders; i++) {
        const rp = Atomics.load(STATE.header, 3 + i);
        if (rp < 0) continue;
        const used = writePos >= rp
            ? writePos - rp
            : STATE.capacity - (rp - writePos);
        if (used > maxUsed) {
            maxUsed = used;
            slowestPos = rp;
        }
    }

    if (slowestPos === writePos) return;  // nothing to drop

    let pos = slowestPos;
    if (pos + 4 > STATE.capacity) {
        pos = 0;
        if (pos === writePos) return;
    }

    const len = readLength(pos);
    if (len === 0 || len > STATE.capacity - 4) {
        // Corrupt — skip all readers at slowestPos to writePos
        for (let i = 0; i < STATE.maxReaders; i++) {
            const rp = Atomics.load(STATE.header, 3 + i);
            if (rp === slowestPos) {
                Atomics.store(STATE.header, 3 + i, writePos);
            }
        }
        return;
    }

    let skipTo = pos + 4 + len;
    if (skipTo >= STATE.capacity) {
        skipTo = 0;
    }

    // Advance ALL readers at the same oldest position
    for (let i = 0; i < STATE.maxReaders; i++) {
        const rp = Atomics.load(STATE.header, 3 + i);
        if (rp >= 0 && rp === slowestPos) {
            Atomics.store(STATE.header, 3 + i, skipTo);
        }
    }
}

/**
 * Write a payload into the MPMC ring buffer, evicting oldest data as needed.
 * Uses slowest active reader for backpressure.
 * @param {Uint8Array} bytes
 * @returns {boolean}
 */
function writePayload(bytes) {
    if (!bytes || bytes.length === 0) {
        return false;
    }

    if (bytes.length > STATE.capacity - 4) {
        return false;
    }

    let writePos = Atomics.load(STATE.header, 0);

    if (writePos + 4 + bytes.length > STATE.capacity) {
        writePos = 0;
    }

    let available = freeBytes(writePos);
    while (available < 4 + bytes.length) {
        dropOldestForSlowest(writePos);
        available = freeBytes(writePos);
    }

    writeLength(writePos, bytes.length);
    STATE.data.set(bytes, writePos + 4);

    let newWrite = writePos + 4 + bytes.length;
    if (newWrite >= STATE.capacity) {
        newWrite = 0;
    }

    Atomics.store(STATE.header, 0, newWrite);
    Atomics.add(STATE.header, 1, 1);       // signal++ (index 1)
    Atomics.notify(STATE.header, 1, 1);    // wake waiters on signal
    return true;
}

/**
 * Append BER length bytes to the output array.
 * @param {number[]} out
 * @param {number} len
 */
function berAppendLength(out, len) {
    if (len < 128) {
        out.push(len);
        return;
    }

    let tmp = len;
    const buf = [];
    while (tmp > 0) {
        buf.push(tmp & 0xff);
        tmp >>= 8;
    }

    out.push(0x80 | buf.length);
    for (let i = buf.length - 1; i >= 0; i -= 1) {
        out.push(buf[i]);
    }
}

/**
 * Append a BER TLV item to the output array.
 * @param {number[]} out
 * @param {number} tag
 * @param {Uint8Array|number[]} valueBytes
 */
function berAppendTlv(out, tag, valueBytes) {
    out.push(tag);
    berAppendLength(out, valueBytes.length);
    for (let i = 0; i < valueBytes.length; i += 1) {
        out.push(valueBytes[i]);
    }
}

/**
 * Append a BER TLV string to the output array.
 * @param {number[]} out
 * @param {number} tag
 * @param {string} value
 */
function berAppendString(out, tag, value) {
    const bytes = new TextEncoder().encode(value);
    berAppendTlv(out, tag, bytes);
}

/**
 * Normalize a record entry into the expected SER shape.
 * @param {Object} entry
 * @returns {{record_id: string, timestamp: string, status: string, description: string}}
 */
function normalizeRecord(entry) {
    const recordId = entry.record_id ?? entry.recordId ?? entry.sno ?? '';
    const timestamp = entry.timestamp ?? `${entry.date ?? ''} ${entry.time ?? ''}`.trim();
    const status = entry.status ?? entry.state ?? '';
    const description = entry.description ?? entry.element ?? '';
    return {
        record_id: String(recordId),
        timestamp: String(timestamp),
        status: String(status),
        description: String(description)
    };
}

/**
 * Encode SER records into a BER TLV payload.
 * @param {Array<Object>} records
 * @returns {Uint8Array}
 */
function encodeSerRecordsToTlv(records) {
    const content = [];

    for (const entry of records) {
        const rec = normalizeRecord(entry);
        const recordValue = [];
        berAppendString(recordValue, 0x80, rec.record_id);
        berAppendString(recordValue, 0x81, rec.timestamp);
        berAppendString(recordValue, 0x82, rec.status);
        berAppendString(recordValue, 0x83, rec.description);

        const recordTlv = [];
        berAppendTlv(recordTlv, 0x30, recordValue);
        content.push(...recordTlv);
    }

    const payload = [];
    berAppendTlv(payload, 0x61, content);
    return new Uint8Array(payload);
}

/**
 * Convert incoming WebSocket data into a ring-buffer payload.
 * @param {string|ArrayBuffer|Blob} data
 * @returns {Promise<void>}
 */
async function handleIncomingData(data) {
    let payload = null;

    if (typeof data === 'string') {
        try {
            const records = JSON.parse(data);
            payload = encodeSerRecordsToTlv(Array.isArray(records) ? records : []);
        } catch (err) {
            self.postMessage({ type: 'error', message: `JSON parse failed: ${err}` });
            return;
        }
    } else if (data instanceof ArrayBuffer) {
        payload = new Uint8Array(data);
    } else if (data && typeof data.arrayBuffer === 'function') {
        const buf = await data.arrayBuffer();
        payload = new Uint8Array(buf);
    }

    if (payload && payload.length) {
        writePayload(payload);
    }
}

/**
 * Post a WebSocket status update to the main thread.
 * @param {string} status
 */
function sendStatus(status) {
    self.postMessage({ type: 'ws_status', status });
}

/**
 * Connect to the WebSocket server and start streaming data.
 * @param {string} url
 */
function connectWebSocket(url) {
    if (!url) {
        return;
    }

    wsUrl = url;

    if (ws) {
        try {
            ws.close();
        } catch (err) {
            // ignore
        }
        ws = null;
    }

    sendStatus('connecting');
    ws = new WebSocket(wsUrl);
    ws.binaryType = 'arraybuffer';

    ws.onopen = () => {
        sendStatus('connected');
        ws.send('getData');
    };

    ws.onmessage = (event) => {
        handleIncomingData(event.data).catch((err) => {
            self.postMessage({ type: 'error', message: String(err) });
        });
    };

    ws.onclose = () => {
        sendStatus('disconnected');
        if (wsUrl) {
            setTimeout(() => connectWebSocket(wsUrl), 3000);
        }
    };

    ws.onerror = () => {
        sendStatus('error');
    };
}

/**
 * Send a command payload over the active WebSocket.
 * @param {string|ArrayBuffer|Uint8Array} payload
 */
function sendWsCommand(payload) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(payload);
    }
}

/**
 * Handle control messages from the main thread.
 * @param {MessageEvent} event
 */
self.onmessage = (event) => {
    const msg = event.data;
    if (!msg || !msg.type) {
        return;
    }

    if (msg.type === 'init') {
        initRing(msg.buffer, msg.capacity);
    } else if (msg.type === 'connect') {
        connectWebSocket(msg.wsUrl);
    } else if (msg.type === 'send') {
        sendWsCommand(msg.payload);
    }
};
