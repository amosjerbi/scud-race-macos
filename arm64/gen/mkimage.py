#!/usr/bin/env python3
"""Pack the initialised parts of scudrace.exe's memory image (headers + sections, minus debug info) into a blob that is
linked into the arm64 binary, so the translated game needs no .exe at run time.
usage: mkimage.py scudrace.exe out.bin"""
import struct, sys
f = open(sys.argv[1], 'rb').read()
o = struct.unpack_from('<I', f, 0x3c)[0]
ns = struct.unpack_from('<H', f, o + 6)[0]; osz = struct.unpack_from('<H', f, o + 20)[0]
opt = o + 24
base = struct.unpack_from('<Q', f, opt + 24)[0]; size = struct.unpack_from('<I', f, opt + 56)[0]; hdrs = struct.unpack_from('<I', f, opt + 60)[0]
chunks = [(0, f[:hdrs])]
for i in range(ns):
    n, vs, va, rs, rp = struct.unpack_from('<8sIIII', f, opt + osz + 40 * i)
    if n.startswith(b'/') or not rs: continue          # DWARF sections, .bss
    chunks.append((va, f[rp:rp + min(rs, vs or rs)]))
out = bytearray(b'SCUDIMG1' + struct.pack('<QII', base, size, len(chunks)))
for va, d in chunks: out += struct.pack('<II', va, len(d)) + d
open(sys.argv[2], 'wb').write(out)
print(f'{len(chunks)} chunks, {len(out) >> 10} KB')
