# av-bridge

A dual-mode bridge that taps the babycam camera's H.264 / G.711 stream and
serves it as a website **from the camera itself** -- no transcoding.

- **Mode A (tee):** when the camera's monitor is up, av-bridge connects to the
  real monitor, forwards the stream to it (the monitor keeps working), and tees
  a copy to the web clients.
- **Mode B (emulate):** when the monitor is down, av-bridge emulates the
  monitor's handshake so ppsapp still streams, and serves that to the web.

The camera (mipsel32, uClibc-0.9.33.2) has no iptables, a read-only rootfs
(but writable `/etc`), and `10.10.10.1` is hardcoded in ppsapp. ppsapp's
launcher also lives on the read-only partition, so we cannot patch its command
line. Instead we redirect ppsapp's outbound `10.10.10.1:11224` connection with a
global `/etc/ld.so.preload` `connect()` interposer that rewrites the destination
to `127.0.0.1:<listen-port>`, where av-bridge is listening. av-bridge itself is
statically linked, so the preload never affects it -- it reaches the real
monitor for Mode A.

## Build

```sh
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work mipsel32-toolchain mipsel-build
```

Produces:
- `build/av-bridge` -- statically linked, web assets embedded.
- `build/hook.so` -- the freestanding `connect()` interposer.

Never build on the host; always use the `mipsel32-toolchain` image.

### Regenerating embedded assets / replies

Run on the host, only when `web/` or the boot pcap changes:

```sh
python3 gen_web_assets.py        # regenerates the embedded website
python3 gen_monitor_replies.py   # regenerates the Mode B monitor handshake replies
```

## Test (QEMU)

```sh
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work mipsel32-toolchain mipsel-build -DBUILD_TESTS=ON
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work mipsel32-toolchain mipsel-run build/tests/av_tests
```

`av_tests` runs 13 unit cases. The Mode A / Mode B end-to-end checks are
one-liners that wire up the server with `fake_ppsapp`, `fake_monitor`, and
`ws_probe` (all built under `build/tools/` with `-DBUILD_TESTS=ON`):

- **Mode A:** start `av-bridge`, start `fake_monitor`, start `fake_ppsapp`, then
  `ws_probe` the web port -- the fake monitor and the web client both receive
  the stream.
- **Mode B:** start `av-bridge` with no monitor reachable, start `fake_ppsapp`,
  then `ws_probe` -- av-bridge emulates the monitor and the web client receives
  the stream.

### hook.so static checks (no QEMU; the make-or-break property)

```sh
docker run --rm -v "$PWD":/work mipsel32-toolchain file build/hook.so
docker run --rm -v "$PWD":/work mipsel32-toolchain bash -c \
  'mipsel-linux-gnu-readelf -d build/hook.so | grep -i needed || echo "NO NEEDED LIBS (good)"; \
   echo "--- connect exported? ---"; mipsel-linux-gnu-readelf --dyn-syms build/hook.so | grep -w connect'
```

Expected: `ELF 32-bit LSB shared object, MIPS`; **NO NEEDED LIBS**; a `connect`
entry (FUNC, not UND) in the dynamic symbol table. Any NEEDED libc means the
freestanding build regressed.

## Deploy

1. TFTP `av-bridge` and `hook.so` to `/mnt/mmc01/testing/` on the camera.
2. `chmod +x av-bridge`
3. Start the bridge:
   ```sh
   ./av-bridge --monitor-ip 10.10.10.1 --listen-port 11225 --web-port 8080 &
   ```
4. Install the hook (it pre-flights and arms a dead-man switch):
   ```sh
   sh install-hook.sh /mnt/mmc01/testing/hook.so
   ```
5. Follow its printed steps: `kill $(pidof ppsapp)` (the watchdog relaunches it
   with the hook), verify `/proc/$(pidof ppsapp)/maps` and `/proc/net/tcp`, open
   a **second** SSH session to prove remote access still works, then
   `kill <deadman-pid>` to keep the hook.
6. Browse to `http://<camera-ip>:8080/`.

## Hook validation (make-or-break)

`install-hook.sh` pre-flights `LD_PRELOAD=hook.so /bin/true`. If the freestanding
glibc-built `.so` will not load into uClibc, the script aborts with **nothing
changed**. (Fallback in that case: build the hook with a uClibc cross-toolchain
and use a normal `dlsym(RTLD_NEXT, "connect")` interposer instead of the
freestanding syscall.)

After install + ppsapp restart:
- `/proc/$(pidof ppsapp)/maps` should list `hook.so`.
- `/proc/net/tcp` should show ppsapp's destination as `0100007F:2BD8`
  (127.0.0.1:11224) instead of `010A0A0A:2BD8` (10.10.10.1:11224).

## SSH safety

`/etc/ld.so.preload` is **global** -- it injects into every dynamically linked
process, including `sshd` and your shell. A bad hook could lock you out. To
prevent that, `install-hook.sh` arms a dead-man switch that auto-empties the
preload file after `N` seconds (default 180) using only shell builtins, so it
fires even if `exec` is broken. **Always verify a second SSH session works
before cancelling the dead-man switch.**

## Notes & limits

- The hook returns `-1` on a failed connect **without setting `errno`** (it has
  no libc/TLS). This is fine for ppsapp's blocking connect.
- HTTP/1.1 + `ws://` only -- no TLS, LAN only.
- A/V sync is approximate.
- High-resolution fragment reassembly is not handled.
- The hook redirect port (`LISTEN_PORT`, default 11225, set via the
  `AVB_LISTEN_PORT` CMake cache var) **must match** the bridge's
  `--listen-port`.
