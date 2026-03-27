// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file ser_worker.js
 * @brief Web Worker that bridges WebSocket to SharedArrayBuffer ring buffer.
 *
 * Binary data (SER TLV payloads) → ring buffer (zero-copy to main thread)
 * Text data (TAR batch, JSON, etc.) → postMessage back to main thread
 * Commands from main thread → forwarded over WebSocket
 *
 * ## Messages: main thread → worker
 *
 * | type         | fields             | description                            |
 * |--------------|--------------------|----------------------------------------|
 * | `init`       | `buffer, capacity` | Attach to SharedArrayBuffer ring       |
 * | `connect`    | `wsUrl`            | Open WebSocket to server               |
 * | `send`       | `payload`          | Forward a command string over WS       |
 * | `disconnect` |                    | Close WS, disable auto-reconnect       |
 *
 * ## Messages: worker → main thread
 *
 * | type        | fields   | description                                     |
 * |-------------|----------|-------------------------------------------------|
 * | `ws_status` | `status` | connecting / connected / disconnected / error   |
 * | `ws_text`   | `data`   | Forwarded text message (TAR, JSON, etc.)        |
 * | `error`     | `message`| Error details                                   |
 *
 * @see ring_buffer.js          Shared ring buffer module
 * @see shared_ring_buffer.hpp  C++ equivalent ring buffer
 * @see asn_tlv_codec.hpp       C++ BER-TLV codec
 * @see combine_ser.js          Dashboard combined SER consumer
 * @see sections.js             Relay detail SER consumer
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

importScripts('ring_buffer.js');

/** @type {RingBufferHandle|null} */
let ring = null;

/** @type {WebSocket|null} */
let ws = null;

/** @type {string|null} */
let wsUrl = null;

/**
 * Post a WebSocket status update to the main thread.
 * @param {string} status
 */
function sendStatus(status) {
    self.postMessage({ type: 'ws_status', status });
}

/**
 * Connect to the WebSocket server.
 * Binary messages go to the ring buffer, text messages are forwarded via postMessage.
 * Auto-reconnects after 3s on close (unless explicitly disconnected).
 * @param {string} url
 */
function connectWebSocket(url) {
    if (!url) return;
    wsUrl = url;

    if (ws) {
        try { ws.close(); } catch (_) { /* ignore */ }
        ws = null;
    }

    sendStatus('connecting');
    ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';

    ws.onopen = () => {
        sendStatus('connected');
    };

    ws.onmessage = (event) => {
        const data = event.data;

        if (data instanceof ArrayBuffer) {
            // Binary TLV → write directly into ring buffer
            if (ring && data.byteLength > 0) {
                ring.writePayload(new Uint8Array(data));
            }
        } else if (typeof data === 'string') {
            // Text → forward to main thread for section-specific handling
            self.postMessage({ type: 'ws_text', data });
        }
    };

    ws.onclose = () => {
        sendStatus('disconnected');
        // Auto-reconnect (wsUrl is cleared on explicit disconnect)
        if (wsUrl) {
            setTimeout(() => connectWebSocket(wsUrl), 3000);
        }
    };

    ws.onerror = () => {
        sendStatus('error');
    };
}

/**
 * Handle control messages from the main thread.
 * @param {MessageEvent} event
 */
self.onmessage = (event) => {
    const msg = event.data;
    if (!msg || !msg.type) return;

    switch (msg.type) {
        case 'init':
            ring = attachRingBuffer(msg.buffer, msg.capacity);
            break;

        case 'connect':
            connectWebSocket(msg.wsUrl);
            break;

        case 'send':
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(msg.payload);
            }
            break;

        case 'disconnect':
            wsUrl = null;   // prevent auto-reconnect
            if (ws) {
                try { ws.close(); } catch (_) { /* ignore */ }
                ws = null;
            }
            sendStatus('disconnected');
            break;
    }
};
