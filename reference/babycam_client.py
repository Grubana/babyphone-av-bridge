#!/usr/bin/env python3
"""
Reference implementation of the "babycam" (ppsapp) control + media protocol.

Verified against captures: babyphone_boot.pcap, babyphone_monitor.pcap.
See babycam_protocol_spec.md for the full protocol reference.

This module provides:
  - Frame            : a parsed protocol frame
  - build_frame()    : encode a frame (matches the device's additive checksum)
  - FrameReader      : incremental parser for a TCP byte stream (handles partial recv)
  - MediaDemuxer     : turns type-8 media frames into an H.264 elementary stream + G.711 audio
  - iter_pcap_frames(): offline helper to replay a pcap (for testing/validation)
  - BabycamListener  : network skeleton for the "base station" role (receives a camera's stream)

The frame codec + demuxer are validated offline; the network role is a documented
skeleton to be confirmed against a live device.
"""
from __future__ import annotations
import socket
import struct
import audioop
from dataclasses import dataclass, field

# ---------------------------------------------------------------------------
# Constants (verified)
# ---------------------------------------------------------------------------
MAGIC = bytes.fromhex("56565099")          # frame start code (binary const @ 0x838318)
HEADER_LEN = 8                              # magic(4) + t0(1) + flag(1) + len(2)
CHECKSUM_LEN = 4

# AES-128 key. key1 and key2 were identical in the dump. Encryption is gated by
# the per-frame low bit of t0 and was NOT exercised in any capture (all plaintext).
KEY1 = bytes([0x06, 0x0a, 0x09, 0x04, 0x04, 0x0a, 0x05, 0x05,
              0x01, 0x01, 0x09, 0x05, 0x0b, 0x0e, 0x08, 0x0f])
KEY2 = KEY1

# Frame types (t0 >> 4)
T_LOGIN     = 1     # initiator -> listener, identity
T_REGISTER  = 2     # device announce / register
T_CONTROL   = 7     # control commands (msg_type_cmd switch)
T_MEDIA     = 8     # video (op 0x0000 keyframe, 0x0100 inter) + audio (op 0x0201)
T_KEEPALIVE_S = 12  # heartbeat from listener
T_KEEPALIVE_C = 13  # keepalive from initiator

# type-8 media ops (first 2 bytes of body)
OP_VIDEO_KEY   = 0x0000   # SPS+PPS+IDR, ~18-byte media header then Annex-B
OP_VIDEO_INTER = 0x0100   # P-slice, ~10-byte media header then Annex-B
OP_AUDIO       = 0x0201   # G.711 mu-law, 20-byte media header then payload (320B = 40ms)

AUDIO_HEADER_LEN = 20     # bytes before G.711 payload in an op-0x0201 body

H264_START = b"\x00\x00\x00\x01"


# ---------------------------------------------------------------------------
# Frame codec
# ---------------------------------------------------------------------------
@dataclass
class Frame:
    type: int
    encrypted: bool
    flag: int
    body: bytes

    @property
    def op(self) -> int:
        return (self.body[0] << 8) | self.body[1] if len(self.body) >= 2 else -1


def additive_checksum(data: bytes) -> int:
    return sum(data) & 0xFFFFFFFF


def build_frame(ftype: int, body: bytes, *, encrypted: bool = False, flag: int = 0x00) -> bytes:
    """Encode a frame exactly as the device expects (big-endian, additive checksum)."""
    if encrypted:
        if len(body) % 16 != 0:
            raise ValueError("encrypted body must be a multiple of 16 bytes")
        key = KEY1 if ftype in (7, 8) else KEY2
        from Crypto.Cipher import AES
        body = b"".join(AES.new(key, AES.MODE_ECB).encrypt(body[o:o + 16])
                        for o in range(0, len(body), 16))
    t0 = ((ftype & 0x0F) << 4) | (1 if encrypted else 0)
    length_field = len(body) + CHECKSUM_LEN
    head = MAGIC + bytes([t0, flag & 0xFF, (length_field >> 8) & 0xFF, length_field & 0xFF])
    frame_wo_ck = head + body
    return frame_wo_ck + struct.pack(">I", additive_checksum(frame_wo_ck))


def parse_frame(buf: bytes, start: int = 0):
    """Try to parse one frame at the first MAGIC at/after `start`.
    Returns (Frame, next_offset) or (None, resume_offset) if more data is needed."""
    s = buf.find(MAGIC, start)
    if s < 0:
        return None, max(start, len(buf) - 3)      # keep last 3 bytes (partial magic)
    if len(buf) - s < HEADER_LEN:
        return None, s
    t0 = buf[s + 4]
    flag = buf[s + 5]
    length_field = (buf[s + 6] << 8) | buf[s + 7]
    total = length_field + 0x0C
    if len(buf) - s < total:
        return None, s                              # incomplete; wait for more bytes
    frame = buf[s:s + total]
    body = frame[HEADER_LEN:total - CHECKSUM_LEN]
    stored = struct.unpack(">I", frame[total - CHECKSUM_LEN:total])[0]
    if additive_checksum(frame[:total - CHECKSUM_LEN]) != stored:
        # bad checksum: skip this magic and resync
        return None, s + 1
    encrypted = bool(t0 & 1)
    if encrypted and body:
        key = KEY1 if (t0 >> 4) in (7, 8) else KEY2
        from Crypto.Cipher import AES
        body = b"".join(AES.new(key, AES.MODE_ECB).decrypt(body[o:o + 16])
                        for o in range(0, len(body) - len(body) % 16, 16))
    return Frame(type=t0 >> 4, encrypted=encrypted, flag=flag, body=body), s + total


class FrameReader:
    """Incremental frame parser for a live TCP stream. Feed bytes, get frames."""
    def __init__(self):
        self._buf = bytearray()

    def feed(self, data: bytes):
        self._buf += data
        out = []
        pos = 0
        while True:
            frame, nxt = parse_frame(self._buf, pos)
            if frame is None:
                pos = nxt
                break
            out.append(frame)
            pos = nxt
        del self._buf[:pos]
        return out


# ---------------------------------------------------------------------------
# Media demuxing
# ---------------------------------------------------------------------------
class MediaDemuxer:
    """Collect type-8 media frames into an H.264 Annex-B stream and G.711 audio.

    Rule: for video frames (op 0x0000 / 0x0100) the H.264 begins at the first
    0x00000001 start code in the body; everything before it is the device media
    header (18 bytes for keyframes, 10 for inter-frames). Audio (op 0x0201) is
    G.711 mu-law (8 kHz mono) starting at body offset 20 (20-byte media header;
    payload is 320 bytes = 40 ms per frame).
    """
    def __init__(self):
        self.h264 = bytearray()
        self.ulaw = bytearray()
        self.n_key = 0
        self.n_inter = 0
        self.n_audio = 0

    def push(self, fr: Frame):
        if fr.type != T_MEDIA:
            return
        op = fr.op
        if op in (OP_VIDEO_KEY, OP_VIDEO_INTER):
            i = fr.body.find(H264_START)
            if i < 0:
                return
            self.h264 += fr.body[i:]
            if op == OP_VIDEO_KEY:
                self.n_key += 1
            else:
                self.n_inter += 1
        elif op == OP_AUDIO:
            self.ulaw += fr.body[AUDIO_HEADER_LEN:]
            self.n_audio += 1

    def write_h264(self, path: str):
        with open(path, "wb") as f:
            f.write(self.h264)

    def write_wav(self, path: str, rate: int = 8000):
        import wave
        pcm = audioop.ulaw2lin(bytes(self.ulaw), 2)
        w = wave.open(path, "wb")
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(pcm); w.close()


# ---------------------------------------------------------------------------
# Offline pcap replay (for testing)
# ---------------------------------------------------------------------------
def iter_pcap_frames(path: str, *, to_port: int = 11224):
    """Yield Frames from the media direction (dest port `to_port`) of a classic pcap."""
    data = open(path, "rb").read()
    assert data[:4] == b"\xd4\xc3\xb2\xa1", "expected classic little-endian pcap"
    off = 24
    segs = {}
    while off + 16 <= len(data):
        _, _, incl, _ = struct.unpack("<IIII", data[off:off + 16]); off += 16
        raw = data[off:off + incl]; off += incl
        if raw[12:14] != b"\x08\x00":
            continue
        ip = raw[14:]; ihl = (ip[0] & 0xF) * 4
        if ip[9] != 6:
            continue
        tcp = ip[ihl:]; sp, dp = struct.unpack(">HH", tcp[:4])
        seq = struct.unpack(">I", tcp[4:8])[0]
        doff = ((tcp[12] >> 4) & 0xF) * 4
        payload = tcp[doff:]
        if dp == to_port and payload:
            segs.setdefault(seq, payload)
    # reassemble by sequence
    stream = bytearray(); nxt = None
    for s in sorted(segs):
        if nxt is None or s >= nxt:
            stream += segs[s]; nxt = s + len(segs[s])
    reader = FrameReader()
    yield from reader.feed(bytes(stream))


# ---------------------------------------------------------------------------
# Network role: base station (receives a camera's stream)
# ---------------------------------------------------------------------------
class BabycamListener:
    """SKELETON (needs live-device confirmation).

    The camera dials OUT to the base station on TCP 11224 and pushes A/V. To
    receive the stream, act as the listener: bind 11224, accept, then read frames.
    Respond to the camera's login with a device-announce, ack control commands,
    and answer heartbeats to keep the stream flowing.
    """
    def __init__(self, bind="0.0.0.0", port=11224):
        self.bind, self.port = bind, port

    def serve_once(self, on_media):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((self.bind, self.port)); srv.listen(1)
        conn, addr = srv.accept()
        reader = FrameReader()
        try:
            while True:
                data = conn.recv(65536)
                if not data:
                    break
                for fr in reader.feed(data):
                    self._handle(conn, fr, on_media)
        finally:
            conn.close(); srv.close()

    def _handle(self, conn, fr: Frame, on_media):
        if fr.type == T_MEDIA:
            on_media(fr)
        elif fr.type == T_KEEPALIVE_S:                     # heartbeat -> echo back
            conn.sendall(build_frame(T_KEEPALIVE_C, b"\x00\x00", flag=0))
        elif fr.type == T_CONTROL:
            # ack the command by echoing msg_type_cmd with an empty body
            if len(fr.body) >= 2:
                conn.sendall(build_frame(T_CONTROL, fr.body[:2], flag=0))
        # T_LOGIN / T_REGISTER handling: send a device-announce; see spec section 7-8.


if __name__ == "__main__":
    import sys
    if len(sys.argv) >= 2:
        dmx = MediaDemuxer()
        for fr in iter_pcap_frames(sys.argv[1]):
            dmx.push(fr)
        dmx.write_h264("out.h264")
        dmx.write_wav("out.wav")
        print(f"keyframes={dmx.n_key} interframes={dmx.n_inter} audio={dmx.n_audio} "
              f"h264={len(dmx.h264)}B ulaw={len(dmx.ulaw)}B")
