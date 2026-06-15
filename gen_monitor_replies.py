#!/usr/bin/env python3
import os, struct
MAGIC = bytes.fromhex("56565099")
def frames(stream):
    pos = 0
    while True:
        s = stream.find(MAGIC, pos)
        if s < 0 or len(stream) - s < 8: break
        t0 = stream[s+4]; lf = (stream[s+6] << 8) | stream[s+7]; total = lf + 0x0C
        if len(stream) - s < total: break
        yield t0 >> 4, stream[s:s+total], stream[s+8:s+total-4]
        pos = s + total
def reassemble(path, src_port=11224):
    d = open(path, 'rb').read(); off = 24; segs = {}
    while off + 16 <= len(d):
        incl = struct.unpack('<I', d[off+8:off+12])[0]; off += 16
        raw = d[off:off+incl]; off += incl
        if raw[12:14] != b'\x08\x00': continue
        ip = raw[14:]; ihl = (ip[0] & 0xF) * 4
        if ip[9] != 6: continue
        tcp = ip[ihl:]; sp = struct.unpack('>H', tcp[:2])[0]; seq = struct.unpack('>I', tcp[4:8])[0]
        doff = ((tcp[12] >> 4) & 0xF) * 4; pl = tcp[doff:]
        if sp == src_port and pl: segs.setdefault(seq, pl)
    out = bytearray(); nxt = None
    for s in sorted(segs):
        if nxt is None or s >= nxt: out += segs[s]; nxt = s + len(segs[s])
    return bytes(out)
def carr(b): return ','.join(str(x) for x in b)
def main():
    here = os.path.dirname(os.path.abspath(__file__))
    stream = reassemble(os.path.join(here, 'test-fixtures', 'babyphone_boot.pcap'))
    announce = None; replies = {}
    for typ, full, body in frames(stream):
        if typ == 2 and announce is None:
            announce = full
        elif typ == 7 and len(body) >= 2:
            cmd = (body[0] << 8) | body[1]
            replies.setdefault(cmd, full)
    out = os.path.join(here, 'src', 'monitor_replies.h')
    with open(out, 'w') as f:
        f.write('#pragma once\n#include <cstdint>\n#include <cstddef>\n\nnamespace monrep {\n')
        f.write(f'static const unsigned char announce[] = {{{carr(announce) if announce else ""}}};\n')
        f.write(f'static const size_t announce_len = {len(announce) if announce else 0};\n')
        items = sorted(replies.items())
        for cmd, full in items:
            f.write(f'static const unsigned char reply_{cmd:04x}[] = {{{carr(full)}}};\n')
        f.write('struct Reply { uint16_t cmd; const unsigned char* data; size_t len; };\n')
        f.write('static const Reply kReplies[] = {\n')
        for cmd, full in items:
            f.write(f'  {{0x{cmd:04x}, reply_{cmd:04x}, {len(full)}}},\n')
        f.write('};\n')
        f.write(f'static const size_t kReplyCount = {len(items)};\n}}\n')
    print(f"announce={len(announce) if announce else 0}B replies={len(replies)} -> {out}")
if __name__ == '__main__': main()
