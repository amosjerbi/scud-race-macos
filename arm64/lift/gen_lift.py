#!/usr/bin/env python3
"""p-code (Ghidra) -> C static recompiler for scudrace.exe.

Input : pcode.txt written by ghidra/ExportPcode.java
Output: out/lift_NNN.c (functions), out/lift_decls.h (prototypes), out/lift_table.c (entry address -> function)

Model: one `void f_<addr>(uint8_t *restrict R)` per Ghidra function. R is the x86-64 register file in Ghidra's register-space layout
(RAX at +0, RSP at +0x20, XMM0 at +0x1200, flags at +0x200 ...). Guest memory is accessed at its real addresses (the PE image is mapped at
0x140000000 in the host). CALL = C call (the guest stack still gets the return address pushed by the p-code), RETURN = C return.
"""
import sys, re, os, collections

SRC, OUT = sys.argv[1], sys.argv[2]
NPER = int(os.environ.get('LIFT_FUNCS_PER_FILE', '120'))
os.makedirs(OUT, exist_ok=True)

# functions replaced by hand-written host versions (x87 libm: Rosetta/ARM-hostile and slow when lifted)
# the statically linked x87 libm routines: the host supplies them (src/overrides.c), as the Rosetta build patches them too
DEFAULT_OVERRIDE = 'log2f,log2,nearbyintf,nearbyint,acosf,asinf,atan2f,atan2,fmodf,fmod'
# hooked functions are emitted as f_<addr>_lifted; the host's f_<addr> decides whether to run them (src/overrides.c)
HOOK = {n for n in os.environ.get('LIFT_HOOK', 'scud_public_env_lock').split(',') if n}
OVERRIDE = {n for n in os.environ.get('LIFT_OVERRIDE', DEFAULT_OVERRIDE).split(',') if n}
ONLY = os.environ.get('LIFT_ONLY')           # comma list of function names: lift just these (feasibility tests)
ONLY = set(ONLY.split(',')) if ONLY else None

UT = {1: 'u8', 2: 'u16', 4: 'u32', 8: 'u64', 16: 'u128'}
ST = {1: 's8', 2: 's16', 4: 's32', 8: 's64', 16: 's128'}
PU = {1: 'u32', 2: 'u32', 4: 'u32', 8: 'u64', 16: 'u128'}      # promoted unsigned type for arithmetic (avoid signed-int overflow UB)
PS = {1: 's32', 2: 's32', 4: 's32', 8: 's64', 16: 's128'}
FT = {4: 'float', 8: 'double'}

def parse_vn(s):
    sp, off, sz = s.split(':')
    return (sp, int(off, 16), int(sz))

class Ins:
    __slots__ = ('addr', 'len', 'mn', 'ops', 'jt')
    def __init__(s, addr, ln, mn): s.addr, s.len, s.mn, s.ops, s.jt = addr, ln, mn, [], None

funcs = []   # (entry, name, [Ins])
cur = None; ci = None
for line in open(SRC):
    if line.startswith('O '):
        p = line.split()
        user = None
        if p[-1].startswith('#'): user = p[-1][1:]; p = p[:-1]
        ci.ops.append((p[1], None if p[2] == '-' else parse_vn(p[2]), [parse_vn(x) for x in p[3:]], user))
    elif line.startswith('I '):
        p = line.split(); ci = Ins(int(p[1], 16), int(p[2]), p[3]); cur[2].append(ci)
    elif line.startswith('J'):
        ci.jt = [int(x, 16) for x in line.split()[1:]]
    elif line.startswith('F '):
        p = line.split(); cur = (int(p[1], 16), p[2], []); funcs.append(cur)

entries = {f[0] for f in funcs}
names = {f[0]: f[1] for f in funcs}
print('parsed', len(funcs), 'functions', file=sys.stderr)

def lift_function(entry, name, inss):
    L = []
    addrs = {i.addr for i in inss}
    # branch targets that need labels
    need = set()
    for i in inss:
        for (op, o, ins, u) in i.ops:
            if op in ('BRANCH', 'CBRANCH') and ins[0][0] == 'm' and ins[0][1] in addrs: need.add(ins[0][1])
        if i.jt: need.update(a for a in i.jt if a in addrs)
    need.add(entry)
    for idx, i in enumerate(inss):
        last = i.ops[-1][0] if i.ops else ''
        nxt = inss[idx + 1].addr if idx + 1 < len(inss) else None
        if last not in ('BRANCH', 'BRANCHIND', 'RETURN') and i.addr + i.len != nxt and (i.addr + i.len) in addrs: need.add(i.addr + i.len)
    usize = 16
    maxu = 0
    body = []
    for idx, i in enumerate(inss):
        nxt = inss[idx + 1].addr if idx + 1 < len(inss) else None
        # compact the unique varnodes of this instruction
        ur = []
        for (op, o, ins, u) in i.ops:
            for v in ([o] if o else []) + ins:
                if v[0] == 'u': ur.append((v[1], v[1] + v[2]))
        ur.sort(); regions = []
        for a, b in ur:
            if regions and a < regions[-1][1]: regions[-1][1] = max(regions[-1][1], b)
            else: regions.append([a, b, 0])
        base = 0
        for r in regions:
            r[2] = base; base += (r[1] - r[0] + 15) & ~15
        maxu = max(maxu, base)
        def umap(off):
            for r in regions:
                if r[0] <= off < r[1]: return r[2] + off - r[0]
            raise KeyError(off)
        def lv(v):
            sp, off, sz = v
            if sp == 'r': return f'(*(RT{sz}*)(L+0x{off:x}))'
            if sp == 'u': return f'(*(RT{sz}*)(U+0x{umap(off):x}))'
            if sp == 'm': return f'(*(RT{sz}*)0x{off:x}ULL)'
            raise ValueError(v)
        def rv(v, t=None):
            sp, off, sz = v
            if sp == 'c':
                if sz == 16: return f'((u128)0x{off:x}ULL)'
                return f'(({UT.get(sz, "u64")})0x{off:x}ULL)'
            return lv(v)
        def f(v):     # value of a float varnode as C float/double
            sz = v[2]
            if sz == 4: return f'F4({rv(v)})'
            if sz == 8: return f'F8({rv(v)})'
            if sz == 10: return f'F80D(&{lv(v)})'
            raise ValueError(v)
        def setf(o, expr):
            sz = o[2]
            if sz == 4: return f'{lv(o)} = B4({expr});'
            if sz == 8: return f'{lv(o)} = B8({expr});'
            if sz == 10: return f'DF80(&{lv(o)}, {expr});'
            raise ValueError(o)
        lab = []
        if i.addr in need: lab.append(f'L_{i.addr:x}:;')
        body.extend(lab)
        # intra-instruction p-code labels
        plab = {}
        for k, (op, o, ins, u) in enumerate(i.ops):
            if op in ('BRANCH', 'CBRANCH') and ins[0][0] == 'c':
                rel = ins[0][1]
                if rel >= 1 << (8 * ins[0][2] - 1): rel -= 1 << (8 * ins[0][2])
                plab[k + rel] = f'P_{i.addr:x}_{k + rel}'
        endk = len(i.ops)
        terminal = False
        for k, (op, o, ins, u) in enumerate(i.ops):
            if k in plab: body.append(f'{plab[k]}:;')
            def A(n=0): return rv(ins[n])
            def T(n): return ins[n][2]
            so = o[2] if o else 0
            dst = lv(o) if o else None
            e = None
            if op == 'COPY':
                if so == 10: e = f'__builtin_memcpy(&{dst}, &{lv(ins[0])}, 10);' if ins[0][0] != 'c' else f'{dst} = 0;'
                else: e = f'{dst} = {A()};'
            elif op == 'LOAD':
                if so == 10: e = f'__builtin_memcpy(&{dst}, (void*)(uintptr_t){A(1)}, 10);'
                else: e = f'{dst} = LD({UT[so]}, {A(1)});'
            elif op == 'STORE':
                sv = ins[2]
                if sv[2] == 10: e = f'__builtin_memcpy((void*)(uintptr_t){A(1)}, &{lv(sv)}, 10);'
                else: e = f'ST({UT[sv[2]]}, {A(1)}, {rv(sv)});'
            elif op in ('INT_ADD', 'INT_SUB', 'INT_MULT', 'INT_AND', 'INT_OR', 'INT_XOR'):
                sym = {'INT_ADD': '+', 'INT_SUB': '-', 'INT_MULT': '*', 'INT_AND': '&', 'INT_OR': '|', 'INT_XOR': '^'}[op]
                pt = PU[so]; e = f'{dst} = ({UT[so]})(({pt}){A(0)} {sym} ({pt}){A(1)});'
            elif op == 'INT_LEFT':
                pt = PU[so]; bits = so * 8
                e = f'{dst} = ({UT[so]})((u64){A(1)} >= {bits} ? 0 : ({pt}){A(0)} << (u64){A(1)});'
            elif op == 'INT_RIGHT':
                pt = PU[so]; bits = so * 8
                e = f'{dst} = ({UT[so]})((u64){A(1)} >= {bits} ? 0 : ({UT[so]}){A(0)} >> (u64){A(1)});'
            elif op == 'INT_SRIGHT':
                bits = so * 8; s = ST[so]
                e = f'{dst} = ({UT[so]})(({s}){A(0)} >> ((u64){A(1)} >= {bits} ? {bits - 1} : (u64){A(1)}));'
            elif op == 'INT_EQUAL': e = f'{dst} = ({UT[T(0)]}){A(0)} == ({UT[T(0)]}){A(1)};'
            elif op == 'INT_NOTEQUAL': e = f'{dst} = ({UT[T(0)]}){A(0)} != ({UT[T(0)]}){A(1)};'
            elif op == 'INT_LESS': e = f'{dst} = ({UT[T(0)]}){A(0)} < ({UT[T(0)]}){A(1)};'
            elif op == 'INT_LESSEQUAL': e = f'{dst} = ({UT[T(0)]}){A(0)} <= ({UT[T(0)]}){A(1)};'
            elif op == 'INT_SLESS': e = f'{dst} = ({ST[T(0)]}){A(0)} < ({ST[T(0)]}){A(1)};'
            elif op == 'INT_SLESSEQUAL': e = f'{dst} = ({ST[T(0)]}){A(0)} <= ({ST[T(0)]}){A(1)};'
            elif op == 'INT_CARRY': e = f'{{ {UT[T(0)]} _r; {dst} = __builtin_add_overflow(({UT[T(0)]}){A(0)}, ({UT[T(0)]}){A(1)}, &_r); }}'
            elif op == 'INT_SCARRY': e = f'{{ {ST[T(0)]} _r; {dst} = __builtin_add_overflow(({ST[T(0)]}){A(0)}, ({ST[T(0)]}){A(1)}, &_r); }}'
            elif op == 'INT_SBORROW': e = f'{{ {ST[T(0)]} _r; {dst} = __builtin_sub_overflow(({ST[T(0)]}){A(0)}, ({ST[T(0)]}){A(1)}, &_r); }}'
            elif op == 'INT_ZEXT': e = f'{dst} = ({UT[so]})({UT[T(0)]}){A(0)};'
            elif op == 'INT_SEXT': e = f'{dst} = ({UT[so]})({ST[so]})({ST[T(0)]}){A(0)};'
            elif op == 'INT_2COMP': e = f'{dst} = ({UT[so]})(-({PS[so]}){A(0)});' if so < 16 else f'{dst} = -{A(0)};'
            elif op == 'INT_NEGATE': e = f'{dst} = ({UT[so]})~{A(0)};'
            elif op in ('INT_DIV', 'INT_REM'):
                t = UT[T(0)]; sym = '/' if op == 'INT_DIV' else '%'
                e = f'if (!({t}){A(1)}) DIVTRAP(); {dst} = ({UT[so]})(({t}){A(0)} {sym} ({t}){A(1)});'
            elif op in ('INT_SDIV', 'INT_SREM'):
                t = ST[T(0)]; sym = '/' if op == 'INT_SDIV' else '%'
                e = f'if (!({t}){A(1)}) DIVTRAP(); {dst} = ({UT[so]})(({t}){A(0)} {sym} ({t}){A(1)});'
            elif op == 'POPCOUNT':
                if T(0) == 16: e = f'{dst} = ({UT[so]})(__builtin_popcountll((u64){A(0)}) + __builtin_popcountll((u64)({A(0)} >> 64)));'
                else: e = f'{dst} = ({UT[so]})__builtin_popcountll((u64)({UT[T(0)]}){A(0)});'
            elif op == 'BOOL_NEGATE': e = f'{dst} = ({UT[so]})({A(0)} ^ 1);'
            elif op in ('BOOL_AND', 'BOOL_OR', 'BOOL_XOR'):
                sym = {'BOOL_AND': '&', 'BOOL_OR': '|', 'BOOL_XOR': '^'}[op]; e = f'{dst} = ({UT[so]})({A(0)} {sym} {A(1)});'
            elif op == 'SUBPIECE':
                sh = ins[1][1] * 8
                e = f'{dst} = 0;' if sh >= T(0) * 8 else (f'{dst} = ({UT[so]})(({UT[T(0)]}){A(0)} >> {sh});' if sh else f'{dst} = ({UT[so]})({UT[T(0)]}){A(0)};')
            elif op == 'PIECE':
                e = f'{dst} = (({UT[so]}){A(0)} << {T(1) * 8}) | ({UT[so]}){A(1)};'
            elif op in ('FLOAT_ADD', 'FLOAT_SUB', 'FLOAT_MULT', 'FLOAT_DIV'):
                sym = {'FLOAT_ADD': '+', 'FLOAT_SUB': '-', 'FLOAT_MULT': '*', 'FLOAT_DIV': '/'}[op]
                if so == 10: e = setf(o, f'{f(ins[0])} {sym} {f(ins[1])}')
                else: e = setf(o, f'{f(ins[0])} {sym} {f(ins[1])}')
            elif op in ('FLOAT_EQUAL', 'FLOAT_NOTEQUAL', 'FLOAT_LESS', 'FLOAT_LESSEQUAL'):
                sym = {'FLOAT_EQUAL': '==', 'FLOAT_NOTEQUAL': '!=', 'FLOAT_LESS': '<', 'FLOAT_LESSEQUAL': '<='}[op]
                e = f'{dst} = {f(ins[0])} {sym} {f(ins[1])};'
            elif op == 'FLOAT_NAN': e = f'{dst} = __builtin_isnan({f(ins[0])});'
            elif op == 'FLOAT_NEG': e = setf(o, f'-{f(ins[0])}')
            elif op == 'FLOAT_ABS': e = setf(o, f'__builtin_fabs({f(ins[0])})')
            elif op == 'FLOAT_SQRT': e = setf(o, f'__builtin_sqrt({f(ins[0])})' if T(0) != 4 else f'__builtin_sqrtf({f(ins[0])})')
            elif op == 'FLOAT2FLOAT': e = setf(o, f'({FT.get(so, "double")}){f(ins[0])}')
            elif op == 'INT2FLOAT': e = setf(o, f'({FT.get(so, "double")})({ST[T(0)]}){A(0)}')
            elif op == 'TRUNC': e = f'{dst} = TRUNC{so}((double){f(ins[0])});'
            elif op == 'ROUND': e = setf(o, f'__builtin_nearbyint({f(ins[0])})')
            elif op == 'BRANCH':
                t = ins[0]
                if t[0] == 'c':
                    rel = t[1]
                    if rel >= 1 << (8 * t[2] - 1): rel -= 1 << (8 * t[2])
                    e = f'goto P_{i.addr:x}_{k + rel};'
                elif t[1] in addrs: e = f'goto L_{t[1]:x};'
                elif t[1] in entries: e = f'{{ SO(); f_{t[1]:x}(R); return; }}'
                else: e = f'{{ SO(); guest_jump(R, 0x{t[1]:x}ULL); return; }}'
            elif op == 'CBRANCH':
                t = ins[0]; c = A(1)
                if t[0] == 'c':
                    rel = t[1]
                    if rel >= 1 << (8 * t[2] - 1): rel -= 1 << (8 * t[2])
                    e = f'if ({c}) goto P_{i.addr:x}_{k + rel};'
                elif t[1] in addrs: e = f'if ({c}) goto L_{t[1]:x};'
                elif t[1] in entries: e = f'if ({c}) {{ SO(); f_{t[1]:x}(R); return; }}'
                else: e = f'if ({c}) {{ SO(); guest_jump(R, 0x{t[1]:x}ULL); return; }}'
            elif op == 'BRANCHIND':
                tv = A(0)
                if i.jt:
                    cases = ' '.join(f'case 0x{a:x}ULL: goto L_{a:x};' for a in sorted(set(i.jt)) if a in addrs)
                    e = f'switch ((u64){tv}) {{ {cases} default: {{ u64 _t = (u64){tv}; SO(); guest_jump(R, _t); return; }} }}'
                else: e = f'{{ u64 _t = (u64){tv}; SO(); guest_jump(R, _t); return; }}'
            elif op == 'CALL':
                t = ins[0]
                e = f'f_{t[1]:x}(R);' if t[1] in entries else f'guest_call(R, 0x{t[1]:x}ULL);'
                if names.get(t[1]) == '_setjmp': e = '{ jmp_buf _jb; rt_cur_jb = &_jb; if (!_setjmp(_jb)) { ' + e + ' } }'   # a real C setjmp in this frame
                e = ('SO(); ' + e + ' SI();') if names.get(t[1]) == '_setjmp' else ('SOC(); ' + e + ' SIC();')
            elif op == 'CALLIND': e = f'{{ u64 _t = (u64){A(0)}; SOC(); guest_call(R, _t); SIC(); }}'
            elif op == 'RETURN': e = 'SOR(); return;'
            elif op == 'CALLOTHER':
                e = userop(u, o, ins, lv, rv, f, setf)
            else:
                e = f'UNSUPPORTED("{op}");'
            body.append(e)
        if len(i.ops) in plab: body.append(f'{plab[len(i.ops)]}:;')
        last = i.ops[-1][0] if i.ops else ''
        if last not in ('BRANCH', 'BRANCHIND', 'RETURN'):
            fall = i.addr + i.len
            if fall != nxt:
                body.append(f'goto L_{fall:x};' if fall in addrs else f'SO(); guest_jump(R, 0x{fall:x}ULL); return;')
                if fall in addrs: need.add(fall)
    # labels discovered late (fallthrough gotos) -> second pass is cheap: emit labels for every `need` that was added after being passed
    # register slots: merged intervals of every register varnode (general registers widened to whole 8-byte registers)
    iv = []; wr = set()
    for i in inss:
        for (op, o, ins, u) in i.ops:
            for v in ([o] if o else []) + ins:
                if v[0] == 'r':
                    a, b = v[1], v[1] + v[2]
                    if a < 0x200: a, b = a & ~7, (b + 7) & ~7
                    iv.append((a, b))
            if o and o[0] == 'r': wr.add(o[1])
    iv.sort(); slots = []
    for a, b in iv:
        if slots and a < slots[-1][1]: slots[-1][1] = max(slots[-1][1], b)
        else: slots.append([a, b])
    lsz = max([b for a, b in slots] + [16])
    def cp(dst, src, a, b):      # in pieces of at most 8 bytes, so stores and later loads of the register file match in size (store forwarding)
        out = []
        while a < b:
            n = 8 if b - a >= 8 else (4 if b - a >= 4 else (2 if b - a >= 2 else 1))
            out.append(f'__builtin_memcpy({dst}+0x{a:x}, {src}+0x{a:x}, {n});'); a += n
        return ' '.join(out)
    si = ' '.join(cp('L', 'R', a, b) for a, b in slots)
    # Win64 calls: the callee only reads argument registers / rsp / x87 state and only hands back rax, rdx, xmm0-1, rsp, x87;
    # callee-saved registers are preserved by the callee, so their up-to-date values simply stay in L
    def ovl(a, b, ivs): return any(a < y and x < b for x, y in ivs)
    ARGS = [(0, 0x18), (0x20, 0x28), (0x80, 0x90), (0x1000, 0x1300)]
    RETS = [(0, 0x8), (0x10, 0x18), (0x20, 0x28), (0x1000, 0x1280)]
    FLAGS = [(0x200, 0x300)]
    dirty = [(a, b) for a, b in slots if any(a <= w < b for w in wr)]
    so = ' '.join(cp('R', 'L', a, b) for a, b in dirty)
    soc = ' '.join(cp('R', 'L', a, b) for a, b in dirty if ovl(a, b, ARGS))
    sic = ' '.join(cp('L', 'R', a, b) for a, b in slots if ovl(a, b, RETS))
    # at a return only what a caller can observe: the result registers, rsp, x87 state and the callee-saved registers
    # (rbx rbp rsi rdi r12-r15 xmm6-15); a caller reloads nothing else (SIC), and longjmp/fibers only use the callee-saved set
    KEEP = RETS + [(0x18, 0x20), (0x28, 0x40), (0xa0, 0xc0), (0x1380, 0x1600)]
    sor = ' '.join(cp('R', 'L', a, b) for a, b in dirty if ovl(a, b, KEEP) and not ovl(a, b, FLAGS))
    decl = (f'#undef SI\n#undef SO\n#undef SOC\n#undef SIC\n#undef SOR\n#define SI() do {{ {si} }} while (0)\n#define SO() do {{ {so} }} while (0)\n'
            f'#define SOC() do {{ {soc} }} while (0)\n#define SIC() do {{ {sic} }} while (0)\n#define SOR() do {{ {sor} }} while (0)\n'
            f'void f_{entry:x}{"_lifted" if name in HOOK else ""}(uint8_t *restrict R) {{ /* {name} */\n  alignas(16) uint8_t U[{max(maxu, 16)}];\n  alignas(16) uint8_t L[{lsz}];\n  SI();\n')
    first = inss[0].addr
    if entry != first: decl += f'  goto L_{entry:x};\n'
    return decl, body, need

def userop(u, o, ins, lv, rv, f, setf):
    a = [rv(x) if x[0] != 'c' else rv(x) for x in ins[1:]]
    if u == 'LOCK': return 'HOST_LOCK();'
    if u == 'UNLOCK': return 'HOST_UNLOCK();'
    if u == 'rdtsc': return f'{lv(o)} = host_rdtsc();'
    if u in ('divps', 'divpd', 'minps', 'maxps', 'packssdw', 'psubusb'): return f'{lv(o)} = U_{u}({a[0]}, {a[1]});'
    if u == 'psllw': return f'{lv(o)} = U_psllw({a[0]}, {a[1]});'
    if u in ('pshuflw', 'pshufhw'): return f'{lv(o)} = U_{u}({a[0]}, {a[1]}, {a[2]});'
    if u in ('fcos', 'fsin', 'f2xm1', 'log2'):
        return f'DF80(&{lv(o)}, U_{u}({f(ins[1])}));'
    if u in ('fpatan', 'fscale'):
        return f'DF80(&{lv(o)}, U_{u}({f(ins[1])}, {f(ins[2])}));'
    if u == 'invalidInstructionException': return 'UD2TRAP();'
    return f'UNSUPPORTED_USEROP("{u}");'

# --------------------------------------------------------------------------- emit
sel = [fn for fn in funcs if fn[1] not in OVERRIDE and (ONLY is None or fn[1] in ONLY)]
decls = []
tab = []
fileno = 0
buf = []
def flush():
    global buf, fileno
    if not buf: return
    with open(f'{OUT}/lift_{fileno:03d}.c', 'w') as fo:
        fo.write('#include "lift_rt.h"\n#include "lift_decls.h"\n')
        fo.write('\n'.join(buf))
    fileno += 1; buf = []
for n, (entry, name, inss) in enumerate(sel):
    decl, body, need = lift_function(entry, name, inss)
    # second pass for labels created by fallthrough gotos: re-run once if needed (rare)
    lines = [decl]
    addrs = {i.addr for i in inss}
    for b in body: lines.append('  ' + b)
    lines.append('}\n')
    txt = '\n'.join(lines)
    buf.append(txt)
    if (n + 1) % NPER == 0: flush()
flush()
decls = [f'void f_{e:x}(uint8_t *restrict R);' for (e, nm, _) in funcs] + [f'void f_{e:x}_lifted(uint8_t *restrict R);' for (e, nm, _) in funcs if nm in HOOK]   # overridden ones are defined by the host
open(f'{OUT}/lift_decls.h', 'w').write('#pragma once\n#include "lift_rt.h"\n' + '\n'.join(decls) + '\n')
tab = sorted((e, nm) for (e, nm, _) in funcs if ONLY is None or nm in ONLY)
with open(f'{OUT}/lift_table.c', 'w') as ft:
    ft.write('#include "lift_rt.h"\n#include "lift_decls.h"\nconst struct lift_ent lift_table[] = {\n')
    for e, nm in tab: ft.write(f'  {{0x{e:x}ULL, f_{e:x}}},\n')
    ft.write('};\nconst unsigned lift_table_n = %d;\n' % len(tab))
print('emitted', len(sel), 'functions in', fileno, 'files', file=sys.stderr)
