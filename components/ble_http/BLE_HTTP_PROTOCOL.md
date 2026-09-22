# WiCAN Pro: the HTTP API over BLE (`ble_http` protocol reference)

> The wire contract of the `http` stream channel (FFF3 notify or indicate /
> FFF4 write, service 0xFFF0), **protocol version 2** (2026-09-22: a body
> frame counter and credits in both directions; v1 frames are refused).
> Subscribe FFF3 for NOTIFICATIONS for speed (then honour section 4b) or
> for INDICATIONS for the simplest client. Read `ble_manager/BLE_API.md` first for
> discovery, pairing, MTU and the stream-channel conventions; this file
> covers only what rides inside that channel. Every route documented in
> `components/HTTP_API.md` is reachable through it, unchanged: the device
> replays your request against its own web server over loopback and
> streams the answer back. Facts here are the constants in
> `ble_http_core.h` and are exercised by the host suite
> (`ble_http/host_test`) and the bench (`tools/testbench/ble/ble_http_pi.py`).

## 1. Why a tunnel

A phone app should not need WiFi to manage the device. Instead of a
second, BLE-only API that would lag behind the HTTP one, the device
carries the SAME requests over BLE: method, path, optional body in,
status, content type, body out. Storage management (`/api/fs/*`), the UDS
terminal (`/api/uds/*`), settings, status, logs, AutoPID, scripts and OTA
all work the day they exist on HTTP. Nothing in `ble_http` knows any
route; a new `/api/*` endpoint needs no BLE work.

What does NOT go through: anything outside `/api/` (the web UI assets,
`/ws/*` WebSocket upgrades). BLE apps have the raw data pipe (FFF1/FFF2)
and the CLI pipe for streaming needs.

## 2. Framing

Every message in either direction is a frame: an 8-byte little-endian
header and a payload of 0 to 4096 bytes. A frame may span several ATT
writes or notifications, and one ATT unit may carry the tail of one frame
and the head of the next: reassemble by `len`, never by packet boundaries.

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 1 | magic | `0x57` (`'W'`) |
| 1 | 1 | version | `0x02` |
| 2 | 1 | type | see below |
| 3 | 1 | flags | bit 0 = `LAST` (final body frame); bits 7..4 = **body frame counter** on `REQ_BODY` / `RSP_BODY`: 0 on the first body frame of a request or response, +1 per frame modulo 16, the `LAST` frame included. Other bits 0 |
| 4 | 2 | seq | request id, chosen by the app, echoed in every frame of that exchange |
| 6 | 2 | len | payload bytes that follow, 0..4096 |

| Type | Name | Direction | Payload |
|---|---|---|---|
| 1 | `REQ` | app to device | JSON request head |
| 2 | `REQ_BODY` | app to device | request body bytes |
| 3 | `RSP` | device to app | JSON response head |
| 4 | `RSP_BODY` | device to app | response body bytes |
| 5 | `ABORT` | app to device | none (`len` 0); allowed during the body AND during the response |
| 6 | `CREDIT` | both | `u32` LE, cumulative: device to app = request body bytes accepted so far; app to device = response body bytes received so far (notify mode) |

A device that reads a bad magic or version skips forward byte by byte to
the next `0x57` (`resync` counter in `blehttp`); an app that does the same
recovers from any stray byte. Frame types the device does not expect
(`RSP`, `RSP_BODY` from the app) are ignored.

**The counter is the loss detector.** BLE's link layer never loses a
packet, but a device or phone stack can drop one it already accepted
(measured on the device: the controller dropped PDUs the host had counted
when its heap-allocated ACL TX buffer failed, since fixed with static
buffers; on a Pi: BlueZ's D-Bus notification path dropped whole PDUs
under load). In
notify mode every `RSP_BODY` frame is exactly one notification, so a
lost PDU is a lost whole frame and the next frame's counter shows the
gap. Check it: a gap means the response is torn; discard it and repeat
the request. The device does the same on `REQ_BODY` (`400 hole`).

## 3. A request

1. Send `REQ` with the head:

   ```json
   {"m":"GET","p":"/api/fs/list?path=/data","ct":"","len":0}
   ```

   | Key | Meaning |
   |---|---|
   | `m` | `GET`, `POST`, `PUT` or `DELETE` |
   | `p` | path + query, must start with `/api/`, printable ASCII, percent-encode anything else (do NOT encode the slashes of a `path=` value: the fs API validates paths literally) |
   | `ct` | request `Content-Type` (optional; `application/json` for settings PUTs, `application/octet-stream` for raw uploads) |
   | `len` | request body bytes that follow (optional, default 0, max 8 MB) |

2. If `len` > 0, send the body as `REQ_BODY` frames carrying the same
   `seq`, in order, each up to 4096 bytes, counters 0, 1, 2, ... (mod 16),
   `LAST` set on the final one. The device forwards them as they arrive
   (streamed, nothing is staged).

3. Read the answer: one `RSP` head

   ```json
   {"s":200,"ct":"application/json","len":142}
   ```

   (`len` is the upstream `Content-Length`, or `-1` when the route
   streams a chunked response), then zero or more `RSP_BODY` frames and
   ALWAYS one final `RSP_BODY` with `LAST` set (usually empty), counters
   0, 1, 2, ... across them. Treat `LAST` as the end of the response,
   `len` as advisory, a counter gap as loss.

One request in flight per link. A second `REQ` while one is active is
answered `{"s":429,"err":"busy"}` (+ its `LAST`) and the first continues.

## 4. Flow control for uploads

BLE write-without-response has no acknowledgement, so a fast phone can
outrun the device's receive buffer (32 KB; the credit window is 16 KB). Two safe ways to upload:

- **Write with response** (`WRITE_TYPE_DEFAULT` / `.withResponse`): each
  write is acknowledged at the ATT layer; nothing else to do. Slower
  (one write per connection event).
- **Write without response + credits**: keep at most 16 KB of body bytes
  unacknowledged. The device sends a `CREDIT` frame every 8 KB it has
  accepted (payload = total accepted bytes, `u32` LE); advance your
  window from it. The final response follows the last body frame.

A body frame the device could not store (buffer full) is dropped and the
request fails with `{"s":400,"err":"hole"}` (the counter gap); simply
retry with a smaller window.

## 4b. Flow control for downloads: notify mode

How the device sends `RSP_BODY` depends on the CCCD you wrote on FFF3:

| CCCD | Mode | Frames | Your duty | Speed (bench, 7.5 ms interval) |
|---|---|---|---|---|
| `0x0002` | **indications** | up to 4096 B, several PDUs each | none: your stack confirms every PDU, the device sends the next after the confirmation | one 490 B PDU per two connection intervals: ~28 KB/s at 7.5 ms, 2-5 KB/s at a phone's 30-45 ms |
| `0x0001` | **notifications** | one frame per PDU: payload <= `min(490, MTU - 3) - 8` (482 at MTU 517) | send `CREDIT` frames (cumulative `RSP_BODY` payload bytes received) at least every 8 KB; the device keeps at most **16 KB** unacknowledged and stops until the next credit; check the counter | the radio's rate: ~60 KB/s on 1M, ~120 KB/s on 2M, independent of the connection interval |

Both bits set (`0x0003`) = notifications. Android apps write the CCCD
themselves (`ENABLE_NOTIFICATION_VALUE` / `ENABLE_INDICATION_VALUE`);
iOS `setNotifyValue(true)` and BlueZ pick notifications when the
characteristic offers them, so they are in notify mode and MUST send
credits. A client that never credits stalls a download after 16 KB and
the device closes it 30 s later with `LAST` (fewer bytes than `len`,
`timeouts` +1 in `blehttp`). Sending a credit every 4 KB keeps the window
from ever running dry. Credits must not exceed what you received; the
device ignores such a value. `GET /api/ble` shows the mode the device is
in (`channels[].out`) and how many PDUs went each way
(`tx_notifications`, `tx_indications`).

## 5. Errors the tunnel itself generates

These come as an `RSP` head with `err` instead of `ct`/`len`, followed by
an empty `LAST` body frame:

| `s` | `err` | When |
|---|---|---|
| 400 | `bad_request` | head is not JSON / missing `m` or `p` / oversized |
| 400 | `method` | `m` not in GET/POST/PUT/DELETE |
| 403 | `path` | `p` outside `/api/`, contains whitespace or control chars, `/../` or `//` |
| 400 | `len` | body bytes exceed the announced `len`, or `len` invalid |
| 400 | `short` | `LAST` arrived before `len` bytes |
| 400 | `hole` | a `REQ_BODY` counter gap: a body PDU never reached the device (buffer full on write-without-response, or lost in your stack) |
| 429 | `busy` | a request is already in flight (uploading or still streaming its response) |
| 499 | `aborted` | you sent `ABORT` during the body |
| 502 | `upstream` | the loopback request could not be opened / read |
| 504 | `timeout` | no body frame for 30 s during an upload |

An `ABORT` while the response is streaming, or 30 s without a `CREDIT`
in notify mode, ends the response with its `LAST` frame instead (the head
already went out): you see fewer bytes than `len`.

Everything else, including 404 / 409 / 503 from the routes themselves,
is the route's own response passed through unchanged (`s` = HTTP status,
body = the JSON the HTTP API documents).

## 6. Worked examples (byte-exact)

Header bytes are shown first, then the payload.

**GET /api/status** (seq 1, FFF3 on indications: 4096 B frames):

```
app  -> FFF4  57 02 01 00 01 00 1C 00 | {"m":"GET","p":"/api/status"}
dev  -> FFF3  57 02 03 00 01 00 2E 00 | {"s":200,"ct":"application/json","len":1093}
dev  -> FFF3  57 02 04 00 01 00 45 04 | ...1093 bytes of JSON...   (counter 0, one frame)
dev  -> FFF3  57 02 04 11 01 00 00 00 |                            (counter 1 + LAST, empty)
```

The same on notifications (MTU 517): the body comes as three frames of
482, 482 and 129 bytes (flags `00`, `10`, `20`) and the empty LAST has
flags `31`; after the third frame the app owes nothing yet (1093 B < the
4 KB credit step), so no `CREDIT` goes back.

**Storage: list, make a folder, upload 64 KB raw, download it back, delete**

```
{"m":"GET","p":"/api/fs/list?path=/data"}                       -> 200 {"path":"/data","entries":[...]}
{"m":"GET","p":"/api/fs/info?path=/sd"}                         -> 200 {"total":N,"used":N}  (503 while no card)
{"m":"POST","p":"/api/fs/mkdir?path=/data/app"}                 -> 200 {"ok":true}
{"m":"POST","p":"/api/fs/upload?path=/data/app/cfg.bin",
 "ct":"application/octet-stream","len":65536}
   + 16 REQ_BODY frames of 4096 bytes (LAST on the 16th)
   <- CREDIT 8192, 16384, ... 57344 (every 8 KB)
   -> 200 {"ok":true,"size":65536,"path":"/data/app/cfg.bin"}
{"m":"GET","p":"/api/fs/download?path=/data/app/cfg.bin"}       -> 200 ct application/octet-stream len 65536
   indications:   <- 16 RSP_BODY frames of 4096 + LAST
   notifications: <- 136 RSP_BODY frames of 482 (counters 0..15, 0..15, ...) + LAST
                  -> CREDIT 4096, 8192, ... every 4 KB you consume (the device
                     pauses whenever 16 KB are unacknowledged)
{"m":"DELETE","p":"/api/fs/file?path=/data/app/cfg.bin"}        -> 200 {"ok":true}
```

The File Manager's other calls map the same way: `GET /api/status` for
`bits.sdcard_mounted` (is a card mounted), `GET /api/logger` for the file
the data logger currently holds open (its download answers 409 `file in
use`). Full route reference: `filesystem/HTTP_API.md`.

**Settings** (reboot-to-apply, like over WiFi):

```
{"m":"GET","p":"/api/settings/wifi_manager"}                     -> the document (secrets masked)
{"m":"PUT","p":"/api/settings/wifi_manager","ct":"application/json","len":N} + body
                                                                 -> 200, persisted, pending_reboot
{"m":"POST","p":"/api/settings/submit"}                          -> 200, the device reboots (the link drops)
```

**UDS over BLE** (the UDS terminal, `HTTP_API.md` 6e11):

```
{"m":"POST","p":"/api/uds/request","ct":"application/json","len":52}
  + body {"tx_id":"7E0","rx_id":"7E8","data":"22 F1 90"}
  -> 200 {"ok":true,"response":"62 F1 90 31 57 43 41 4E ...","positive":true,"elapsed_ms":23,...}
{"m":"POST","p":"/api/uds/session","ct":"application/json","len":51}
  + body {"action":"begin","tx_id":"7E0","rx_id":"7E8"}     -> tester-present session, Exclusive-bus hold
{"m":"GET","p":"/api/uds"}                                      -> "session_active":true
{"m":"POST","p":"/api/uds/session","ct":"application/json","len":16} + {"action":"end"}
```

One UDS request per call, a BLE round trip on top of the bus time
(20 to 30 ms on the native ISO-TP backend). For raw PDUs, filters and
periodic messages use the J2534 channel instead (`J2534_WIRE_PROTOCOL.md`).

**OTA** works the same way (`POST /api/ota/upload`, raw body): uploads
run at 60-120 KB/s on a 7.5 ms link (a 2 MB image in 20-40 s) and the
device reboots at the end, dropping the link. Keep the window rule and
expect `LAST` only after the write completed.

## 7. Client reassembly (pseudo-code)

```python
buf = bytearray()
def on_notify(chunk):                       # FFF3 notification or indication
    buf.extend(chunk)
    while len(buf) >= 8:
        magic, ver, typ, flags, seq, ln = struct.unpack_from("<BBBBHH", buf)
        if magic != 0x57 or ver != 2: del buf[0]; continue     # resync
        if len(buf) < 8 + ln: return                            # wait
        payload = bytes(buf[8:8 + ln]); del buf[:8 + ln]
        deliver(typ, flags, seq, payload)

def request(method, path, body=b"", ct="", notify=True):
    seq = next_seq()
    head = json.dumps({"m": method, "p": path, "ct": ct, "len": len(body)})
    write(frame(1, 0, seq, head.encode()))
    for n, i in enumerate(range(0, len(body), 4096)):
        last = i + 4096 >= len(body)
        flags = ((n & 15) << 4) | (1 if last else 0)            # counter + LAST
        write(frame(2, flags, seq, body[i:i + 4096]))           # honour CREDIT if no-response
    rsp = await frame_of_type(3, seq); out = bytearray(); expect = 0; paid = 0
    while True:
        t = await frame_of_type(4, seq)
        if t.flags >> 4 != expect: raise StreamHole()           # a PDU was lost: retry the request
        expect = (expect + 1) & 15
        out += t.payload
        if t.flags & 1: return json.loads(rsp.payload), bytes(out)
        if notify and len(out) - paid >= 4096:                  # notify mode: pay the device
            paid = len(out); write(frame(6, 0, seq, struct.pack("<I", paid)))
```

`write()` splits a frame into ATT writes of at most `min(490, MTU - 3)`
bytes (`BLE_API.md` 4.3). The device's PDUs are at most that size too;
in notify mode every PDU is one whole frame.

## 8. Limits and behaviour

| | |
|---|---|
| Frame payload | 4096 bytes |
| Request head | path + query < 256 chars, content type < 64 chars |
| Request body | up to 8 MB per request, streamed |
| Concurrency | one request per link; the tunnel serialises |
| Upload idle timeout | 30 s without a body frame ends the request (504) |
| Download credit timeout | notify mode: 30 s without a `CREDIT` while 16 KB are unacknowledged ends the response with `LAST` |
| Download window | notify mode: 16 KB of `RSP_BODY` unacknowledged, frames of `min(490, MTU - 3) - 8` bytes; indications: 4096 B frames, no credits |
| Receive buffer | 32 KB on the device (the credit window is 16 KB: the extra absorbs a window still in flight while the device waits for your stack to confirm a CREDIT indication, which it does only after your queued writes went out) |
| Response | streamed as read from the route; `LAST` closes it even when the route failed mid-body |
| Throughput | bounded by the BLE link, not the tunnel. Measured with the ECU simulator as central (MTU 517, DLE 251, 7.5 ms interval, `TESTING.md` row `blethru`): uploads 60-120 KB/s; downloads on **notifications** run at the same radio rate on either PHY, downloads on **indications** at one 490 B PDU per two connection intervals (~28 KB/s at 7.5 ms, 2-5 KB/s at a phone's 30-45 ms). Notify mode exists since protocol v2 because the frame counter makes the one known failure (a PDU dropped inside a stack after it was accepted) detectable instead of silent; bench downloads of 512 KB show 0 holes |
| Link loss | the active request is dropped on the device; nothing is retried. In notify mode up to 16 KB of a response may be in flight when a link drops; repeat the request |
| Web server sharing | the device's web server handles ONE request at a time. A tunnelled upload or download runs at BLE speed inside a route handler, so for the duration of a long transfer (a 512 KB file is minutes on the bench link) WiFi clients of the web UI wait or time out. Short requests (status, settings, UDS) are not noticeable. Planned improvement: stage request bodies and responses in PSRAM so the handler runs at loopback speed |
| Security | the channel is only served on an encrypted, MITM-authenticated (paired) link; the route handlers see the request as local (`X-WiCAN-Transport: ble`) |

## 9. Observability

- `blehttp` (console / CLI pipe): enabled, registered, link + OUT mode,
  active request, counters (requests, responses, errors, aborts,
  timeouts, resync, bytes in/out, last status, v2: `holes` = `REQ_BODY`
  counter gaps, `credits rx`, `stalls` = download pauses on the window).
- `GET /api/ble` `channels[name=http]`: `out` (`indicate|notify|none`),
  `out_modes` (`both`), `tx_notifications`, `tx_indications`.
- `GET /api/settings/ble_http`: `enabled` (default true), `cli`. Off =
  the FFF3/FFF4 characteristics are not in the GATT table.
- Logs: tag `ble_http`, I at start, W on refusals, never E.
