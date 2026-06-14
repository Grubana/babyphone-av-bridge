# Babycam (ppsapp) Protocol — Implementation Specification

A complete, implementation-ready reference for the TCP control + media protocol used
by the `ppsapp` baby-monitor camera. Reconstructed from binary analysis
(`pps_protocol_babycamera_unpack` @ 0x4a602c, `find_one_packet` @ 0x4a44dc,
`find_start_code` @ 0x4a5bd8, `sub_4a59ac` dispatcher, `pps_protocol_package2struct`
@ 0x4a7514) and validated against two packet captures.

A working reference of the verified core ships alongside this doc as
**`babycam_client.py`** (frame codec, incremental reader, A/V demuxer, pcap replay,
network skeleton). Build on it.

**Status tags:** **[C]** verified against capture + code · **[I]** inferred (layout
known, semantics not) · **[?]** needs live-device confirmation.

---

## 0. What "client" means here

The camera is the **initiator**: it opens a TCP connection *outbound* to the base
station and **pushes** its audio/video up. The base station is the **listener**
(TCP server on port 11224). In the captures the initiator was `10.10.10.253`
(the camera) and the listener was `10.10.10.1` (the monitor/base).

Therefore, to **receive and decode the camera's stream**, your client must play the
**listener / base-station role**:

1. Bind and listen on TCP **11224**.
2. Accept the camera's inbound connection.
3. Read framed messages; the camera begins pushing type-8 media almost immediately.
4. Respond to keep the stream alive: answer heartbeats, ack control commands, and
   send a device-announce in response to the camera's login.
5. Demux the type-8 frames into H.264 + G.711 and play/record.

Roles are defined by **behavior**, not IP — anchor your implementation on "who sends
media" (initiator → listener) rather than on addresses.

---

## 1. Transport

- **TCP**, listener port **11224**. Camera connects out to it. **[C]**
- One connection multiplexes everything (control, video, audio), all using the same
  framing, interleaved. **[C]**
- The camera may open a connection, immediately RST it, and reconnect — tolerate a
  probe/reconnect. **[I]**
- Payloads span TCP segments; you MUST buffer and frame incrementally (see §3, and
  `FrameReader` in the reference). **[C]**

---

## 2. Frame format (wire)

All multi-byte header fields are **big-endian**.

```
offset  size  field
0       4     magic = 56 56 50 99
4       1     t0   :  type = t0 >> 4 ,  encrypted = t0 & 1
5       1     flag (per-message; 0 in all captured control frames)
6       2     length_field
8       N     body         (N = body_len)
8+N     4     checksum     (big-endian u32)
```

- **Total frame length** = `length_field + 0x0C`.
- **body_len** = `length_field - 4`.
- **Checksum** = `sum(all bytes from magic through end of body) & 0xFFFFFFFF`
  (a plain additive sum, big-endian on the wire). Reject on mismatch. **[C]**

**Reader algorithm (mandatory):** scan for magic → ensure ≥ 8 bytes for the header →
read `length_field` → ensure ≥ `total` bytes buffered → validate checksum → emit
frame, advance by `total`. If short, keep bytes and wait for more. On checksum
failure, skip one byte past the magic and resync. (`parse_frame` / `FrameReader`.)

---

## 3. Encryption

- AES-128-**ECB**, re-keyed per 16-byte block, gated **per frame** by `t0 & 1`. **[C]**
- Body must be a multiple of 16 when encrypted. **[C]**
- Key select: **key1 if `type ∈ {7,8}`, else key2**. Both keys are identical in this
  device: `06 0a 09 04 04 0a 05 05 01 01 09 05 0b 0e 08 0f`. **[C]**
- **Not exercised in any capture** — every observed frame (control, video, audio) has
  the encrypt bit clear. Implement the path, but expect plaintext. **[C]**

---

## 4. Frame types & dispatch

`t0 >> 4` selects the handler (`sub_4a59ac` routes types 2/7/8/0xC/0xE to a parser
plus a registered callback). Observed types: **[C]**

| type | meaning |
|-----:|---------|
| 1 | login / identity (initiator → listener) |
| 2 | device announce / register |
| 7 | control commands (the `msg_type_cmd` switch) |
| 8 | media — video (op 0x0000 / 0x0100) and audio (op 0x0201) |
| 12 | heartbeat (from listener) |
| 13 | keepalive (from initiator) |

For types 7/2/0xC/0xE the body begins with a 16-bit **`msg_type_cmd`** and the parser
switches on **`msg_type_cmd - 1`** (an off-by-one). **[C]**

### Control body layout (types 7/2/0xC/0xE)

```
0  2  msg_type_cmd (big-endian u16)   -> parser case = msg_type_cmd - 1
2  1  flag / direction (0x01 = initiator->listener in captures)
3  2  sequence / echo (big-endian u16)
5  .. command-specific fields (fixed per command)
```

Acks echo the same `msg_type_cmd` back with a 2-byte (command-only) body. **[C]**

---

## 5. Control command catalog (type 7)

Valid wire commands are **0x01–0x30** (parser rejects `cmd-1 >= 0x30`). "parsed len"
is the decoded-struct size the parser reports. **[C]** for the table mechanics;
per-command *meaning* is **[I]** unless noted.

| wire cmd | case | parsed len | meaning / notes |
|---------:|-----:|-----------:|-----------------|
| 0x03 | 2 | 6 | **LOGIN** payload lives in a **type-1** frame: 10-digit id + 6-byte MAC **[C]** |
| 0x05 | 4 | 2 | config (`01 01 1c …`) **[C]** |
| 0x06 | 5 | 0xC | **STREAM_CFG** `01 01 01 04 00` (codec/resolution?) **[C]** |
| 0x08 | 7 | 8 | **CH_ENABLE** `01 00 00 00` **[C]** |
| 0x0A | 9 | 0x86 | **CLIENT_VER** — `01 00 00` + ASCII version (e.g. `5.7.1-20250412`) **[C]** |
| 0x0D | 0xC | 4 | **START / enable** `01 …` **[C]** |
| 0x11 | 0x10 | 2 | command-only; seen S→C with a 0x80-byte zero blob **[C]** |
| 0x17 | 0x16 | 2 | **SET_PARAM** (carries `… 64 01 2c 46 28`) **[C]** |
| 0x1C | 0x1B | 2 | **TIMESTAMP / keepalive** (carries u32) **[C]** |
| 0x1D | 0x1C | 6 | **QUERY** `00 00` **[C]** |
| 0x1F | 0x1E | 2 | **ENABLE** `01 01 01` **[C]** |
| 0x24 | 0x23 | 0x128 | **DEV_INFO** — request/response; reply carries id + version + `-upgrade…` string **[C]** |
| 0x25 | 0x24 | 0x66 | b,u16 + 0x20 + 0x40 blobs **[I]** |
| 0x27 | 0x26 | 2 | **POWER_STATUS** — JSON `{"elec_status":N,"is_charge":N}` **[C]** |
| 0x28 | 0x27 | n+4 | u16-length-prefixed string → `pps_malloc_w(n)` + `strncpy` **[C]** |
| 0x29 | — | — | **DROPPED** (no `case 0x28`) — silently ignored despite being sent at boot **[C]** |
| 0x26 | — | — | **DROPPED** (no `case 0x25`) **[C]** |

Other commands (0x01,02,04,07,09,0B,0C,0E,0F,10,12–16,18–23,2A–30) parse into
fixed-width structs of known size but unknown semantics — see the parser switch for
exact field widths; full table in `babycam_setup_protocol.md`. **[I]**

### Sharp edges
- **Dropped 0x26 / 0x29:** missing switch cases; these commands no-op. The app sends
  `0x0029` (`00 00 00 1d 00 01 b7 60`) during boot — it is ignored on this path. **[C]**
- **0x28 alloc:** `pps_malloc_w(n)` + `strncpy` with a wire-controlled 16-bit length,
  no visible upper bound — handle carefully if you implement the camera side. **[C]**

---

## 6. Login (frame type 1)

Parsed by the `arg2==1` branch (not the `msg_type_cmd` switch). **[C]**
Body (after the 8-byte frame header) carries `msg_type_cmd = 0x0003`, then:
10-digit ASCII numeric id + 6-byte MAC. Example: id `1260479000`, MAC
`06:56:29:9c:88:38`. Decoded struct is 0x2A bytes (u16 + byte + 0x20 blob + MAC). **[C]**

---

## 7. Device announce (frame type 2)

Fixed-width, zero-padded, big-endian. Offsets within the **body**: **[C]**

```
+0x00  3    sub-header (00 00 01 — record type/version?)        [I]
+0x03  32   device id / serial  (ASCII, "ppsm3003b3a37b3a4547")
+0x23  64   auth token          (32 ASCII hex, MD5-shaped, "fad0…dc7")
+0x63  20   numeric id          (ASCII, "126014925")
+0x77  ~11  binary counters/rates (two big-endian u32 + padding)  [I]
+0x81  6    MAC address         (raw bytes)
+0x87  rest firmware version    (ASCII, null-padded, "babymonitor-a3dap5-2.1.2.20250306")
```

> Parse by **fixed offsets**, never by scanning for ASCII — the MAC's trailing byte
> sits right before the version string and a scan glues it on as a phantom char.

To act as the listener, you must **send** an announce in response to the camera's
login. Reusing the camera's own announced identity is the safe default until the
exact required fields are confirmed. **[?]**

---

## 8. Media (frame type 8) — THE STREAM

Three ops, distinguished by the first 2 body bytes: **[C]**

| op | content | media header | codec |
|----|---------|--------------|-------|
| 0x0000 | **video keyframe** | 18 bytes | H.264 SPS(7)+PPS(8)+IDR(5), Annex-B |
| 0x0100 | **video inter-frame** | 10 bytes | H.264 P-slice (NAL type 1), Annex-B |
| 0x0201 | **audio** | **20 bytes** | G.711 µ-law, 8 kHz mono |

### The universal video rule (use this — robust)

For **any** video frame (op 0x0000 or 0x0100): **the H.264 begins at the first
`00 00 00 01` start code in the body.** Everything before it is the device media
header. This sidesteps the (partially unknown) header fields entirely. Concatenate
the H.264 from each video frame, in arrival order, into one Annex-B elementary
stream. One frame = one access unit at 640×360 (no fragmentation observed; the
type-8 reassembly path in `package2struct` using `data_a1b488`/`data_a3b488` only
kicks in for larger payloads — **[I]** for high-res modes). **[C]**

### Audio

G.711 **µ-law, 8 kHz, mono**. Payload = `body[20:]` (fixed **20-byte** media header);
each frame carries exactly 320 bytes = 40 ms. Decode µ-law → 16-bit PCM and wrap as
WAV, or mux directly. **[C]**

> **Critical:** the audio header is 20 bytes, not 12. Stripping the wrong amount feeds
> header bytes into the µ-law decoder as fake samples on every frame and the result is
> noise (a 25 Hz artifact), even though RMS looks "healthy." Verify any audio decode by
> **lag-1 autocorrelation** of the linear PCM — correct G.711 here scores ~0.98; a wrong
> codec/offset scores near 0. RMS alone does **not** distinguish signal from noise.

### Media header fields (partially decoded, not needed for playback) **[I]**

```
keyframe (op 0x0000), 18-byte header, e.g. 00 00 74 67 00 91 d9 a6 00 6a 2f 1e db 8f 80 00 16 0f
inter    (op 0x0100), 10-byte header, e.g. 01 00 74 63 00 91 d8 17 03 90
```
Contains incrementing frame counters and timestamps; values not required if you use
the start-code rule. To name them fully, read the camera-side frame-builder (the
`IMP_Encoder_GetStream` consumer that emits ops 0x0000/0x0100/0x0201).

### Confirmed stream parameters (this device) **[C]**
- Video: **H.264 Main profile (`27 4d 00 33` = Main, level 5.1), 640×360, yuv420p**
- Frame rate: **~10 fps**
- GOP: **~38 frames (~3.8 s)** — a short capture can miss keyframes
- Audio: **G.711 µ-law, 8 kHz, mono**

---

## 9. Observed session / setup sequence (boot capture)

Camera = initiator (I), Base = listener (L). **[C]**

1. I→L: **login** (type 1, `0x0003`) — id + MAC
2. L→I: **device announce** (type 2)
3. L→I: ack / **DEV_INFO** (`0x0024`) reply (id + version + `-upgrade…`)
4. I→L: config/status burst — `0x0024` query, `0x001d`, `0x0029`(dropped), `0x0017`,
   `0x000d`, **`0x0027` power JSON**, `0x0008`, `0x000b`, `0x0006`, `0x001f`,
   `0x0005`, **`0x000a` client version**, `0x001c`
5. I→L: **media begins** — interleaved type-8 video (0x0000 keyframes + 0x0100
   inter) and audio (0x0201)
6. L→I: periodic **heartbeat** (type 12, `00 01`); I→L **keepalive** (type 13, `00 00`)

**Minimal viable listener:** the camera starts pushing media right after connecting,
so a minimal client can: accept → send announce → demux media → answer heartbeats.
The full control choreography may not be strictly required to receive the stream;
confirm against the device. **[?]**

---

## 10. Implementation plan (for Claude Code)

Build on `babycam_client.py` (verified core). Suggested structure:

1. **Frame layer** — `FrameReader.feed(bytes) -> [Frame]`. Already handles partial
   TCP reads, checksum validation, resync, and optional decryption. Reuse as-is.
2. **Listener** — `BabycamListener` skeleton: bind 11224, accept, pump `recv` into
   `FrameReader`. Fill in `_handle` for login → announce, control → ack, heartbeat →
   reply. Mark these reply paths as needing device testing.
3. **Demuxer** — `MediaDemuxer.push(frame)`; writes `out.h264` + `out.wav`. For live
   playback, feed the H.264 to a decoder (ffmpeg/pyav) and the µ-law to an audio sink.
4. **Mux to MP4** (offline/record): `ffmpeg -r 10 -i out.h264 -i out.wav -c:v libx264
   -c:a aac -shortest out.mp4`. Verified working.
5. **Validate offline first** — `python babycam_client.py babyphone_monitor.pcap`
   should report `keyframes=15 interframes=553 audio=1418`. Use the pcaps as
   regression fixtures before touching the network.

### Pitfalls (each cost time during RE — don't repeat)
- Frames span TCP segments → never parse per-recv; always buffer (the reader does).
- Video is **not** all one op: 0x0000 = keyframes (with SPS/PPS), 0x0100 = inter only.
  A capture with no 0x0000 frame cannot decode (no parameter sets / IDR).
- "type 8 = encrypted" is **false**; encryption is the per-frame `t0 & 1` bit.
- Don't AES-decrypt media unless `t0 & 1` is set, or you'll turn plaintext into noise.
- Audio and video ops were initially swapped — 0x0201 is **audio**, 0x0100 is video.
- Device header lengths differ by op (18 vs 10 vs 12); the start-code rule avoids
  hardcoding them for video.
- **Audio header is 20 bytes** (op 0x0201), not 12. Wrong offset = noise, not glitchy
  speech. Validate audio with **lag-1 autocorrelation** (~0.98 when right), never RMS —
  RMS can't tell signal from noise.

---

## 11. Open items for live confirmation

1. Exact fields the camera requires in the listener's **device-announce** and which
   control acks (if any) are mandatory to start/sustain the stream. **[?]**
2. The 18/10-byte media header field meanings (counter vs timestamp vs flags). **[I]**
3. Type-8 **fragment reassembly** for higher-resolution / larger frames
   (`data_a1b488` / `data_a3b488` path). **[I]**
4. Per-command semantics for the **[I]** rows in §5.
5. Whether encryption is ever enabled in other modes (then validate key1/key2). **[?]**
6. The initial connect-then-RST: required, or an app artifact. **[I]**

---

## Appendix A — constants

```
MAGIC      = 56 56 50 99
KEY1=KEY2  = 06 0a 09 04 04 0a 05 05 01 01 09 05 0b 0e 08 0f
PORT       = 11224 (TCP, listener/base side)
checksum   = additive sum of (magic..body), big-endian u32
total_len  = length_field + 0x0C
video      = H.264 Annex-B, starts at first 00 00 00 01 in a type-8 op 0x0000/0x0100 body
audio      = G.711 mu-law 8kHz mono, body[20:] of a type-8 op 0x0201 frame (320B=40ms)
```

## Appendix B — files
- `babycam_client.py` — verified reference (codec, reader, demuxer, pcap replay, skeleton)
- `babycam_setup_protocol.md` — earlier handshake-focused notes + full control table
- `babycam_proto.py` — minimal frame codec used during analysis
- pcaps `babyphone_boot.pcap`, `babyphone_monitor.pcap` — regression fixtures
