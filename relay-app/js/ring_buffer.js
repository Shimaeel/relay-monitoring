// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file ring_buffer.js
 * @brief SharedArrayBuffer-backed MPMC ring buffer (writer + reader).
 *
 * @description Reusable ring buffer module shared between:
 * - **shm_worker.js** (writer side — receives WS data, writes into ring)
 * - **index.html**    (reader side — reads from ring, renders Tabulator table)
 *
 * ## Memory Layout
 *
 * ```
 * SharedArrayBuffer:
 * ┌──────────────────────────────────────────────────────────────┐
 * │ Int32[0]: writePos                                          │
 * │ Int32[1]: signal (notification counter for Atomics.wait)    │
 * │ Int32[2]: readerCount                                       │
 * │ Int32[3..10]: per-reader readPos (-1 = inactive)            │
 * ├──────────────────────────────────────────────────────────────┤
 * │ Uint8[headerBytes .. headerBytes + capacity]: data ring     │
 * │   [4-byte LE len][payload bytes][4-byte LE len][payload]... │
 * └──────────────────────────────────────────────────────────────┘
 * ```
 *
 * @see shared_ring_buffer.hpp  C++ equivalent implementation
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

// ─── Constants ───────────────────────────────────────────────────────

const RING_MAX_READERS = 8;
const HEADER_SLOTS = 3 + RING_MAX_READERS;  // writePos + signal + readerCount + 8 readPos
const HEADER_BYTES = HEADER_SLOTS * 4;

// ─── Create (for main thread — allocates SharedArrayBuffer) ──────────

/**
 * Create a new SharedArrayBuffer-backed ring buffer.
 * Call from the main thread, then pass `ring.buffer` to workers.
 *
 * @param {number} capacity  Data area size in bytes.
 * @returns {RingBufferHandle}
 */
function createRingBuffer(capacity) {
    const buffer = new SharedArrayBuffer(HEADER_BYTES + capacity);
    const header = new Int32Array(buffer, 0, HEADER_SLOTS);

    Atomics.store(header, 0, 0);   // writePos
    Atomics.store(header, 1, 0);   // signal
    Atomics.store(header, 2, 0);   // readerCount
    for (let i = 0; i < RING_MAX_READERS; i++) {
        Atomics.store(header, 3 + i, -1);  // -1 = inactive
    }

    return _makeHandle(buffer, capacity);
}

// ─── Attach (for workers — receives existing SharedArrayBuffer) ──────

/**
 * Attach to an existing SharedArrayBuffer (already initialised).
 * Call from a worker after receiving the buffer via postMessage.
 *
 * @param {SharedArrayBuffer} buffer  The shared buffer from createRingBuffer().
 * @param {number} capacity           Data area size in bytes.
 * @returns {RingBufferHandle}
 */
function attachRingBuffer(buffer, capacity) {
    return _makeHandle(buffer, capacity);
}

// ─── Internal: build the handle object ───────────────────────────────

/**
 * @typedef {Object} RingBufferHandle
 * @property {SharedArrayBuffer} buffer
 * @property {number} capacity
 * @property {Int32Array} header
 * @property {function(): number} registerReader
 * @property {function(number): void} unregisterReader
 * @property {function(number): Uint8Array|null} readPayload
 * @property {function(Uint8Array): boolean} writePayload
 */

/** @private */
function _makeHandle(buffer, capacity) {
    const header = new Int32Array(buffer, 0, HEADER_SLOTS);
    const data   = new Uint8Array(buffer, HEADER_BYTES, capacity);
    const view   = new DataView(buffer, HEADER_BYTES, capacity);

    // ── Low-level helpers ────────────────────────────────────────

    function readLength(pos) {
        return view.getUint32(pos, true);
    }

    function writeLength(pos, len) {
        view.setUint32(pos, len, true);
    }

    function slowestReaderPos(writePos) {
        let slowest = writePos;
        let maxUsed = 0;
        let anyActive = false;

        for (let i = 0; i < RING_MAX_READERS; i++) {
            const rp = Atomics.load(header, 3 + i);
            if (rp < 0) continue;
            anyActive = true;
            const used = writePos >= rp
                ? writePos - rp
                : capacity - (rp - writePos);
            if (used > maxUsed) {
                maxUsed = used;
                slowest = rp;
            }
        }

        return anyActive ? slowest : writePos;
    }

    function freeBytes(writePos) {
        const slowReadPos = slowestReaderPos(writePos);
        if (writePos >= slowReadPos) {
            return capacity - (writePos - slowReadPos);
        }
        return slowReadPos - writePos;
    }

    function dropOldestForSlowest(writePos) {
        let slowestPos = writePos;
        let maxUsed = 0;

        for (let i = 0; i < RING_MAX_READERS; i++) {
            const rp = Atomics.load(header, 3 + i);
            if (rp < 0) continue;
            const used = writePos >= rp
                ? writePos - rp
                : capacity - (rp - writePos);
            if (used > maxUsed) {
                maxUsed = used;
                slowestPos = rp;
            }
        }

        if (slowestPos === writePos) return;

        let pos = slowestPos;
        if (pos + 4 > capacity) {
            pos = 0;
            if (pos === writePos) return;
        }

        const len = readLength(pos);
        if (len === 0 || len > capacity - 4) {
            for (let i = 0; i < RING_MAX_READERS; i++) {
                const rp = Atomics.load(header, 3 + i);
                if (rp === slowestPos) {
                    Atomics.store(header, 3 + i, writePos);
                }
            }
            return;
        }

        let skipTo = pos + 4 + len;
        if (skipTo >= capacity) {
            skipTo = 0;
        }

        for (let i = 0; i < RING_MAX_READERS; i++) {
            const rp = Atomics.load(header, 3 + i);
            if (rp >= 0 && rp === slowestPos) {
                Atomics.store(header, 3 + i, skipTo);
            }
        }
    }

    // ── Public API ───────────────────────────────────────────────

    /**
     * Register a new reader. Returns readerId (0..7) or -1 if full.
     */
    function registerReader() {
        const writePos = Atomics.load(header, 0);
        for (let i = 0; i < RING_MAX_READERS; i++) {
            const old = Atomics.compareExchange(header, 3 + i, -1, writePos);
            if (old === -1) {
                Atomics.add(header, 2, 1);
                return i;
            }
        }
        return -1;
    }

    /**
     * Unregister a reader, freeing its slot.
     */
    function unregisterReader(readerId) {
        if (readerId < 0 || readerId >= RING_MAX_READERS) return;
        Atomics.store(header, 3 + readerId, -1);
        Atomics.sub(header, 2, 1);
    }

    /**
     * Read next payload for a specific reader.
     * Returns Uint8Array payload or null if nothing available.
     */
    function readPayload(readerId) {
        if (readerId < 0 || readerId >= RING_MAX_READERS) return null;

        const readPos = Atomics.load(header, 3 + readerId);
        if (readPos < 0) return null;

        const writePos = Atomics.load(header, 0);
        if (readPos === writePos) return null;

        let pos = readPos;
        if (pos + 4 > capacity) {
            pos = 0;
        }

        let len = readLength(pos);
        if (len === 0 || len > capacity - 4) {
            Atomics.store(header, 3 + readerId, writePos);
            return null;
        }

        if (pos + 4 + len > capacity) {
            pos = 0;
            if (pos === writePos) {
                Atomics.store(header, 3 + readerId, pos);
                return null;
            }
            len = readLength(pos);
            if (len === 0 || len > capacity - 4) {
                Atomics.store(header, 3 + readerId, writePos);
                return null;
            }
        }

        const payload = data.slice(pos + 4, pos + 4 + len);
        let newRead = pos + 4 + len;
        if (newRead >= capacity) {
            newRead = 0;
        }
        Atomics.store(header, 3 + readerId, newRead);
        return payload;
    }

    /**
     * Write a payload into the ring buffer, evicting oldest data as needed.
     * @param {Uint8Array} bytes
     * @returns {boolean}
     */
    function writePayload(bytes) {
        if (!bytes || bytes.length === 0) return false;
        if (bytes.length > capacity - 4) return false;

        let writePos = Atomics.load(header, 0);

        if (writePos + 4 + bytes.length > capacity) {
            writePos = 0;
        }

        let available = freeBytes(writePos);
        while (available < 4 + bytes.length) {
            dropOldestForSlowest(writePos);
            available = freeBytes(writePos);
        }

        writeLength(writePos, bytes.length);
        data.set(bytes, writePos + 4);

        let newWrite = writePos + 4 + bytes.length;
        if (newWrite >= capacity) {
            newWrite = 0;
        }

        Atomics.store(header, 0, newWrite);
        Atomics.add(header, 1, 1);
        Atomics.notify(header, 1, 1);
        return true;
    }

    return {
        buffer,
        capacity,
        header,
        registerReader,
        unregisterReader,
        readPayload,
        writePayload
    };
}
