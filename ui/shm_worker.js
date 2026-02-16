// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file shm_worker.js
 * @brief Web worker bridge between WebSocket input and a shared ring buffer.
 */

/**
 * Shared ring buffer state used by the worker.
 * @type {{header: Int32Array|null, data: Uint8Array|null, view: DataView|null, capacity: number, signalIndex: number}}
 */
const STATE = {
    header: null,
    data: null,
    view: null,
    capacity: 0,
    signalIndex: 2
};

/** @type {WebSocket|null} */
let ws = null;
/** @type {string|null} */
let wsUrl = null;

/**
 * Initialize the ring buffer views for the shared buffer.
 * @param {ArrayBuffer|SharedArrayBuffer} buffer
 * @param {number} capacity
 */
function initRing(buffer, capacity) {
    STATE.header = new Int32Array(buffer, 0, 3);
    STATE.data = new Uint8Array(buffer, 12, capacity);
    STATE.view = new DataView(buffer, 12, capacity);
    STATE.capacity = capacity;
}

/**
 * Compute free bytes available in the ring buffer.
 * @param {number} readPos
 * @param {number} writePos
 * @returns {number}
 */
function freeBytes(readPos, writePos) {
    if (writePos >= readPos) {
        return STATE.capacity - (writePos - readPos);
    }
    return readPos - writePos;
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
 * Drop the oldest record to free space.
 * @param {number} readPos
 * @param {number} writePos
 * @returns {number}
 */
function dropOldest(readPos, writePos) {
    if (readPos === writePos) {
        return readPos;
    }

    if (readPos + 4 > STATE.capacity) {
        readPos = 0;
        if (readPos === writePos) {
            return readPos;
        }
    }

    const len = readLength(readPos);
    if (len === 0 || len > STATE.capacity - 4) {
        return writePos;
    }

    let newRead = readPos + 4 + len;
    if (newRead >= STATE.capacity) {
        newRead = 0;
    }
    return newRead;
}

/**
 * Write a payload into the ring buffer, evicting oldest data as needed.
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

    let readPos = Atomics.load(STATE.header, 1);
    let writePos = Atomics.load(STATE.header, 0);

    if (writePos + 4 + bytes.length > STATE.capacity) {
        writePos = 0;
    }

    let available = freeBytes(readPos, writePos);
    while (available < 4 + bytes.length) {
        readPos = dropOldest(readPos, writePos);
        available = freeBytes(readPos, writePos);
    }

    writeLength(writePos, bytes.length);
    STATE.data.set(bytes, writePos + 4);

    let newWrite = writePos + 4 + bytes.length;
    if (newWrite >= STATE.capacity) {
        newWrite = 0;
    }

    Atomics.store(STATE.header, 1, readPos);
    Atomics.store(STATE.header, 0, newWrite);
    Atomics.add(STATE.header, STATE.signalIndex, 1);
    Atomics.notify(STATE.header, STATE.signalIndex, 1);
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
