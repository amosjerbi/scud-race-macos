#!/usr/bin/env python3
"""prof.py <binary> <prof file> [function]: per-function sample counts; with a function, its annotated disassembly"""
import bisect, collections, subprocess, sys
b, pf = sys.argv[1], sys.argv[2]
syms = []
for l in subprocess.check_output(['nm', '-n', b]).decode().splitlines():
    p = l.split()
    if len(p) == 3 and p[1] in 'tT': syms.append((int(p[0], 16), p[2]))
ad = [a for a, _ in syms]
L = open(pf).read().split('\n'); base = int(L[0].split()[1], 16)
c = collections.Counter(); ins = collections.Counter(); tot = 0
for l in L[1:]:
    if not l: continue
    pc = int(l, 16) - base + 0x100000000; tot += 1
    i = bisect.bisect_right(ad, pc) - 1
    if 0 <= i < len(syms) and pc - syms[i][0] < 0x100000: c[syms[i][1]] += 1; ins[pc] += 1
    else: c['(other)'] += 1
if len(sys.argv) < 4:
    for n, k in c.most_common(15): print(f'{k:6d} {100 * k / tot:5.1f}% {n}')
    sys.exit()
d = subprocess.check_output(['objdump', '-d', '--no-show-raw-insn', '--disassemble-symbols=' + sys.argv[3], b]).decode()
for l in d.splitlines():
    try: a = int(l.split(':')[0], 16)
    except ValueError: continue
    print(f'{ins.get(a, 0):5d} {l.strip()}')
