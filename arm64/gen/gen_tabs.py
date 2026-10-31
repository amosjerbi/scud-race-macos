#!/usr/bin/env python3
"""One-shot port step: add a call signature to every import-table entry in src/{k32,crt,misc}.c and regenerate src/sdl_wrap.c.
A signature is "<ret>:<args>" with letters q=64-bit int/pointer, w=32-bit int, h=16-bit, b=8-bit, f=float, d=double, v=void (ret only),
V = variadic shim that takes a pointer to the Win64 argument slots."""
import re, json, subprocess, sys, os
src = 'src'
files = ['k32.c', 'crt.c', 'misc.c']
text = {f: open(f'{src}/{f}').read() for f in files}

def split_top(s):
    out = []; d = 0; cur = ''
    for ch in s:
        if ch in '([': d += 1
        if ch in ')]': d -= 1
        if ch == ',' and d == 0: out.append(cur.strip()); cur = ''
        else: cur += ch
    if cur.strip(): out.append(cur.strip())
    return out

def clean(t):
    t = re.sub(r'\b(const|volatile|MS|static|struct|restrict|__restrict)\b', ' ', t)
    return re.sub(r'\s+', ' ', t).strip()

protos = {}   # name -> (ret, [params])
pat = re.compile(r'^(?:static\s+)?(?:MS\s+)?([A-Za-z_][\w \t\*]*?[\s\*])(\w+)\(((?:[^()]|\([^()]*\))*)\)\s*\{', re.M)
for f in files:
    for m in pat.finditer(text[f]):
        ret, name, ps = m.group(1), m.group(2), m.group(3)
        ps = [] if ps.strip() in ('', 'void') else split_top(ps)
        protos[name] = (ret, ps)

types = set()
def ptype(p):
    if '(' in p or '[' in p: return '*'
    p = clean(p)
    if '*' in p or p.endswith('...'): return '*'
    w = p.split(' ')
    if len(w) >= 2 and w[-1] not in ('int', 'long', 'char', 'short', 'unsigned'): w = w[:-1]
    return ' '.join(w)
def rtype(r):
    r = clean(r)
    return '*' if '*' in r else r

sdl = json.load(open('gen/sdl_protos.json'))
SPECIAL = {'SDL_GetWindowWMInfo', 'SDL_GetBasePath', 'SDL_RWFromFile', 'SDL_OpenAudioDevice', 'SDL_JoystickAttachVirtualEx', 'SDL_CreateWindow', 'SDL_RenderPresent', 'SDL_CreateRenderer'}
def ret_of(t):
    d = 0
    for i, ch in enumerate(t):
        if ch == '(' and d == 0: return t[:i].strip()
for n, p in sdl.items():
    if n in SPECIAL or p.get('variadic'): continue
    types.add(rtype(ret_of(p['type'])))
    for q in p['params']:
        if q != 'void': types.add(ptype(q))
for n, (r, ps) in protos.items():
    types.add(rtype(r))
    for q in ps: types.add(ptype(q))
types.discard('*'); types.discard('void')

# classify via the compiler
probe = ['#include "common.h"', '#include <SDL.h>', '#include <stdio.h>', '#include <sys/types.h>', '#include <sys/socket.h>', '#include <netdb.h>', 'typedef uintptr_t SOCKET;',
         '#define P(T) printf("%s|%zu|%d\\n", #T, sizeof(T), __builtin_classify_type(*(T*)0));', 'int main(void){']
tl = sorted(types)
for t in tl: probe.append(f'P({t})')
probe.append('return 0;}')
os.makedirs('build', exist_ok=True)
open('build/probe.c', 'w').write('\n'.join(probe))
sdlcfg = subprocess.check_output(['sdl2-config', '--cflags']).decode().split()
r = subprocess.run(['clang', '-w', '-Isrc'] + sdlcfg + ['build/probe.c', '-o', 'build/probe'], capture_output=True, text=True)
if r.returncode: print(r.stderr[:3000]); sys.exit(1)
cls = {}
for l in subprocess.check_output(['build/probe']).decode().splitlines():
    t, sz, c = l.rsplit('|', 2); cls[t] = (int(sz), int(c))

def letter(t, signed_hint=True):
    if t == '*': return 'q'
    if t == 'void': return 'v'
    sz, c = cls[t]
    if c == 8: return 'f' if sz == 4 else 'd'
    if c == 12: return 'q' if sz == 8 else '?'
    return {8: 'q', 4: 'w', 2: 'h', 1: 'b'}[sz]
def sig(r, ps):
    return letter(rtype(r)) + ':' + ''.join(letter(ptype(p)) for p in ps)

# rewrite the tables
VARIADIC = {'fprintf_ms': 'w:V'}
bad = []
for f in files:
    t = text[f]
    m = re.search(r'const struct import_entry (\w+)\[\] = \{(.*?)\n\};', t, re.S)
    while m:
        body = m.group(2); out = []
        ents = re.findall(r'E2?\(([^()]*(?:\([^()]*\))?[^()]*)\)|\{\s*"(\w+)",\s*([^}]*?)\s*\}', body)
        for a, n2, f2 in ents:
            if a:
                if ',' in a: n, fn = [x.strip() for x in a.split(',', 1)]; n = n.strip('"')
                else: n = fn = a.strip()
            else: n, fn = n2, f2
            if fn.startswith('&') or fn.startswith('(void *)&'):
                out.append(f'    {{ "{n}", (void *){fn.replace("(void *)", "")}, 0 }},')
            else:
                fn = fn.replace('(void *)', '').strip()
                if fn in VARIADIC: s = VARIADIC[fn]
                elif fn in protos: s = sig(*protos[fn])
                else: bad.append((f, n, fn)); continue
                if '?' in s: bad.append((f, n, fn, s)); continue
                out.append(f'    {{ "{n}", (void *){fn}, "{s}" }},')
        newt = f'const struct import_entry {m.group(1)}[] = {{\n' + '\n'.join(out) + '\n    { 0, 0, 0 }\n};'
        t = t[:m.start()] + newt + t[m.end():]
        m = None
        # next table in the same file
        for m2 in re.finditer(r'const struct import_entry (\w+)\[\] = \{(.*?)\n\};', t, re.S):
            if '"' + '' not in m2.group(2) or 'sig' in m2.group(2): continue
            if ', "' in m2.group(2).splitlines()[1] and m2.group(2).splitlines()[1].rstrip().endswith('},') and re.search(r'"[qwhbfdv]:[qwhbfdV]*" \}', m2.group(2)): continue
            m = m2; break
    text[f] = t
for b in bad: print('NO SIG', b)

# sdl wrappers
o = ['/* generated by gen/gen_tabs.py: typed wrappers for SDL2 */', '#include "common.h"', '#include <SDL.h>', '']
tab = []
for name, p in sorted(sdl.items()):
    if name in SPECIAL or p.get('variadic'): continue
    r = ret_of(p['type']); ps = [] if (not p['params'] or p['params'] == ['void']) else p['params']
    s = sig(r, [ps_ for ps_ in ps]) if all(ptype(x) in cls or ptype(x) == '*' for x in ps) else None
    if s is None or '?' in s: print('skip sdl', name, p['type']); continue
    args = []; call = []
    for i, t in enumerate(ps):
        args.append(f'{t} a{i}')
        if re.search(r'(Callback|Function)$', t.replace(' ', '')): call.append(f'({t})rev_thunk((void*)a{i})')
        else: call.append(f'a{i}')
    o.append(f'static {r} w_{name}({", ".join(args) or "void"}) {{ {"return " if r.strip() != "void" else ""}{name}({", ".join(call)}); }}')
    tab.append((name, s))
o += ['', 'const struct import_entry sdl_generated_table[] = {'] + [f'  {{"{n}", (void*)w_{n}, "{s}"}},' for n, s in tab] + ['  {0,0,0}};']
open(f'{src}/sdl_wrap.c', 'w').write('\n'.join(o) + '\n')
for f in files: open(f'{src}/{f}', 'w').write(text[f])
print(len(tab), 'sdl wrappers')
