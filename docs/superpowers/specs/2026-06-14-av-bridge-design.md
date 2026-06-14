# Design: av-bridge — tee the babycam stream to a built-in website

**Date:** 2026-06-14
**Status:** Approved (design phase)

## Goal

Run a small static binary on the babycam camera (mipsel32) that captures the
camera's live H.264 + G.711 stream and serves it as a website hosted on the
camera itself — so a browser on the LAN can watch/listen live. It must work
**whether or not** the original monitor (10.10.10.1) is present.

The camera's `ppsapp` is the initiator: it dials OUT to the monitor on TCP
**11224** and pushes A/V using the framed protocol in
`reference/babycam_protocol_spec.md` (magic `56565099`, additive checksum, type-8 media:
op `0x0000` video keyframe, `0x0100` inter, `0x0201` audio).

## Scope

**In scope:**
- A C++17 static mipsel32 binary (`av-bridge`), built with the existing
  `mipsel32-toolchain` image, deployed like `babyphone-mock`.
- Intercept ppsapp's outbound :11224 connection via an `LD_PRELOAD` `connect()`
  interposer (the device has **no iptables**, and `10.10.10.1` is hardcoded in
  ppsapp, so a syscall-level rewrite is the mechanism).
- **Dual mode**, auto-selected per connection:
  - **Mode A (tee):** real monitor reachable → transparent byte-relay both ways +
    passive media tap. Monitor and website both work.
  - **Mode B (emulate):** monitor unreachable → act as the base station (replay
    the monitor's captured login/announce/control/heartbeat) so the camera keeps
    streaming; tap media for the website.
- A media tap (port of `babycam_client.py`'s `FrameReader` + `MediaDemuxer`).
- An HTTP server (serves a static page + assets) + a minimal WebSocket server
  pushing H.264 NALUs + G.711 µ-law to the browser.
- A browser page: jMuxer → MSE for video; µ-law→PCM→Web Audio for audio.
- The `connect()` interposer `.so` + `install-hook.sh` (adds it to
  `/etc/ld.so.preload`).

**Out of scope:**
- Re-encoding/transcoding (the mipsel CPU can't do it live — remux only).
- TLS for the website (plain `http://` + `ws://` on the LAN — simpler).
- AES of the babycam frames (the bit is honored, but every captured frame is
  plaintext; no key handling needed for playback).
- Frame-accurate A/V sync (a baby monitor tolerates approximate sync).

## Decisions

- **Device:** the same mipsel32 camera we deploy `babyphone-mock` to; root shell,
  writable storage, runs static binaries. **No iptables**; ppsapp is dynamically
  linked against **uClibc-0.9.33.2**; `10.10.10.1` is hardcoded in ppsapp.
- **Capture:** an `LD_PRELOAD` `connect()` interposer (`hook.so`) rewrites
  ppsapp's outbound `10.10.10.1:11224` → `127.0.0.1:<listen>`, installed globally
  via `/etc/ld.so.preload`. The bridge itself is **statically linked**, so it is
  immune to the preload and dials the real monitor unmodified (Mode A). Dual-mode
  A/B chosen per connection.
- **Playback:** WebSocket + MSE (jMuxer, video) + Web Audio (µ-law, audio). No
  server transcoding.
- **Transport security:** plain `http://`/`ws://` (LAN).
- **Build order:** Mode A (tee) → Mode B (emulate) → website. Video before audio.

## Architecture

One binary, structured like `cpp-mock`. Threads: a relay/acceptor, the media
hub fan-out, and the HTTP/WS server, each supervised (restart on throw) with
SIGINT/SIGTERM shutdown.

```
        LD_PRELOAD hook.so rewrites ppsapp's connect(10.10.10.1:11224) -> 127.0.0.1:LP
ppsapp (camera) ───────────────────────────────────────────────▶ relay.accept()
                                                                     │
       on accept, relay dials the real monitor (bridge is static -> not hooked):
         reachable  ─▶ MODE A: splice cam<->mon verbatim; copy cam->mon bytes to tap
         unreachable─▶ MODE B: emulate monitor (replay announce/ctl/heartbeat); tap
                                                                     │
                                            FrameReader ▶ MediaDemuxer ▶ stream_hub
                                                                     │
http+ws server ◀─────────────────────────────────────────────── stream_hub
   browser: jMuxer→MSE (H.264)  +  µ-law→PCM→WebAudio (audio)
```

### Mode selection (per connection)
On each accepted ppsapp connection, dial the real monitor with a short timeout
(e.g. 500 ms). Because the bridge is statically linked, its own `connect()` is
*not* rewritten by the preload, so it reaches the real `10.10.10.1` — no loop:
- **connect ok → Mode A.** Two threads splice bytes verbatim in both directions.
  The cam→mon copy is also fed to the tap. We are protocol-agnostic here; the
  real monitor drives login/announce/heartbeat. Monitor unaffected.
- **connect fails → Mode B.** We answer the camera ourselves: on its login (type
  1), send the captured device-announce (type 2); ack control commands; reply to
  heartbeats (type 12 → keepalive type 13). Media is tapped the same way.

If the monitor dies mid-session (Mode A relay breaks), the connection closes;
ppsapp reconnects and we re-evaluate → Mode B. Transitions ride the camera's
natural reconnect. No mid-session mode switching needed.

### Mode B fidelity — replay captured monitor bytes
`test-fixtures/babyphone_boot.pcap` contains the real monitor's *entire* side of a session
startup: **1 ANNOUNCE (type 2) + 20 CONTROL (type 7) + 3 HEARTBEAT (type 12)**,
and media flows (cam→mon shows 1 keyframe + 33 inter + 85 audio). Mode B is built
by extracting those monitor→camera frames from the capture and replaying them:
- Send the captured type-2 announce immediately after the camera's login.
- For each control command the camera sends, reply with the corresponding
  captured ack/echo (match by `msg_type_cmd`); fall back to the spec's
  "echo `msg_type_cmd` with empty body" for any unmatched command.
- Answer heartbeats with a keepalive (type 13, `00 00`).
This replaces guesswork with the monitor's observed behavior. Exact mandatory
subset is confirmed on-device by watching whether media sustains.

### Components (`av-bridge/`)
- `src/babycam_codec.{h,cpp}` — `parse_frame`/`FrameReader` (magic, additive
  checksum, partial-buffer framing, resync) + `MediaDemuxer` (type-8; H.264 from
  first `00 00 00 01`; G.711 from `body[20:]`). Mirrors `babycam_client.py`.
- `src/relay.{h,cpp}` — accept; dial monitor (plain connect, short timeout); Mode A
  splice + tap, or hand off to Mode B emulator. Tap is a read-only byte copy →
  FrameReader.
- `hook/hook.c` — the `connect()` interposer: a freestanding (`-nostdlib`,
  raw-syscall) `.so` that rewrites ppsapp's `10.10.10.1:11224` connect to
  `127.0.0.1:<listen>` and forwards everything else unchanged. Freestanding so it
  loads into a uClibc process despite our glibc toolchain. Built as a separate
  CMake target.
- `src/monitor_emulator.{h,cpp}` — Mode B: the captured announce + a
  `msg_type_cmd → reply-bytes` table + heartbeat reply. Reply bytes live in a
  committed generated header `src/monitor_replies.h` (a `gen_monitor_replies.py`
  host script extracts the monitor→camera frames from `test-fixtures/babyphone_boot.pcap` into
  C++ byte arrays; the script + its output are committed so the device build needs
  no pcap).
- `src/stream_hub.{h,cpp}` — thread-safe: latest SPS/PPS, a bounded recent-NALU
  buffer, audio buffer; registers WS clients and starts each at a keyframe.
- `src/http_ws_server.{h,cpp}` — HTTP/1.1 (reuse cpp-mock's request parser) for
  the static page + a minimal RFC6455 WebSocket (SHA-1 accept via mbedTLS,
  server→client binary frames; tolerate client close). Each media unit is one WS
  binary message with a 1-byte tag (0=video,1=audio) + payload.
- **Web assets are embedded in the binary** (single self-contained file — avoids
  the per-file TFTP-deploy friction we hit before). `web/index.html`, `web/app.js`,
  `web/jmuxer.min.js` are authored as files, then a host script (`gen_web_assets.py`)
  emits a committed `src/web_assets.h` (path → bytes + content-type); the HTTP
  server serves them from memory.
- `src/main.cpp` — CLI (`--monitor-ip`, `--monitor-port`, `--listen-port`,
  `--web-port`), supervised threads, signals.
- `install-hook.sh` — copies `hook.so` to the device and adds its path to
  `/etc/ld.so.preload`; `uninstall-hook.sh` removes it.

### Redirect mechanism (no iptables)
`hook.so` exports `connect()`. The dynamic linker (uClibc honors
`/etc/ld.so.preload`) binds our `connect` ahead of libc's for every dynamically
linked process. The hook inspects the `sockaddr`: if it is
`AF_INET 10.10.10.1:11224` it copies the address, rewrites it to
`127.0.0.1:<listen>`, and performs the connect via a **raw MIPS syscall**
(`__NR_connect`); all other connects are forwarded unchanged via the same
syscall. No libc symbols are referenced, so the glibc-built `.so` loads cleanly
into the uClibc ppsapp. The **bridge is statically linked**, so the preload does
not apply to it and its dial to the real monitor is never rewritten — no loop,
no marks. Install once via `/etc/ld.so.preload`; ppsapp picks it up on its next
(watchdog) restart.

## Testing

- **Codec unit tests (cross-compiled, run under QEMU):**
  - `build_frame`→`parse_frame` round-trip; multi-frame buffer; partial-buffer
    incremental framing; checksum-mismatch resync.
  - type-8 keyframe: H.264 extracted from the first start code; audio: `body[20:]`.
  - **pcap regression:** a test fixture replays `test-fixtures/babyphone_monitor.pcap`
    (cam→mon direction) through the C++ demuxer and asserts
    **keyframes=15, inter=553, audio=1418** — the exact counts validated with the
    reference. (The pcaps live in `test-fixtures/`, mounted into the build container.)
- **Mode B replay test:** extract the monitor→camera frames from
  `test-fixtures/babyphone_boot.pcap`; assert the emulator's announce + ack table reproduce
  those bytes for the observed `msg_type_cmd`s.
- **Pipeline integration (QEMU):** a fake ppsapp that connects and pushes
  synthesized type-8 frames (keyframe-led), a fake monitor (toggle reachable/
  unreachable to exercise A and B), and a WS client probe that connects and
  asserts it receives a keyframe-led H.264 stream + audio. Covers
  relay+tap+hub+WS end-to-end (everything except the on-device hook).
- **Hook validation (on-device, before full deploy):** install `hook.so` via
  `/etc/ld.so.preload`, confirm it loads into ppsapp (e.g. it appears in
  `/proc/<ppsapp-pid>/maps` after restart) and that `/proc/net/tcp` then shows
  ppsapp connected to `127.0.0.1:<listen>` instead of `10.10.10.1:11224`. This is
  the make-or-break step for the glibc-built freestanding `.so` in uClibc.
- **On-device end-to-end:** run the bridge, confirm (a) monitor up → monitor still
  works and the website plays; (b) monitor down → website still plays. Browser is
  the final check.

## Risks / open items

- **Freestanding `.so` loads into uClibc** — the top risk. A glibc-toolchain build
  with `-nostdlib` + raw syscalls should have no libc deps and load anywhere;
  validated on-device by the hook-validation step. Fallback if it won't load:
  build a matching uClibc cross-toolchain and a normal `dlsym(RTLD_NEXT)` interposer.
- **`/etc/ld.so.preload` is global** — it injects into every dynamically linked
  process (incl. sshd/shell), so a bad `.so` could break remote access. Mitigated
  by: the hook is a strict, crash-proof passthrough that only ever rewrites
  `10.10.10.1:11224` (forwarding all else verbatim via raw syscall); and
  `install-hook.sh` (a) **pre-flight-tests** that the `.so` loads
  (`LD_PRELOAD=hook.so /bin/true`) before touching `/etc`, and (b) arms a
  **dead-man switch** (`( sleep N; : > /etc/ld.so.preload ) &`, builtins only, so
  it self-heals even if `exec` is globally broken) that auto-disables the preload
  unless the operator cancels it after confirming SSH still works. The launcher is
  on the read-only partition, so targeted per-ppsapp injection isn't possible —
  global preload is the chosen mechanism.
- **ppsapp watchdog/restart** — installing via `/etc/ld.so.preload` means the hook
  is picked up on ppsapp's next restart automatically; if no watchdog restarts it,
  restart ppsapp manually once.
- **Mode B mandatory-reply subset** — [?] in the spec; mitigated by replaying
  captured monitor bytes, finalized by on-device observation.
- **jMuxer/MSE** with this exact H.264 (Main, level 5.1, 640×360, ~10 fps) — broadly
  supported; verify in a real browser.
- **A/V sync** between MSE video and Web Audio — approximate by design.
- **Type-8 fragment reassembly** for larger frames (`data_a1b488` path) is [I] /
  unobserved at 640×360 — out of scope unless a high-res mode appears.

## Success criteria

- C++ demuxer reproduces the reference counts on `babyphone_monitor.pcap`
  (15/553/1418).
- Static mipsel32 binary builds and runs under QEMU; the pipeline integration test
  passes for both Mode A and Mode B.
- On-device: the `hook.so` loads into ppsapp and redirects its connection to the
  local bridge; the website plays live video+audio with the monitor present
  (monitor unaffected) AND with the monitor absent.
