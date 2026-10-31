/* PE loader + process entry for the macOS native host. */
#include "common.h"
#include <ucontext.h>
#include <sys/syscall.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/time.h>
#include <signal.h>

uintptr_t g_image_base;
int g_trace;
char g_game_dir[1024];
char g_exe_path_win[1100];
volatile struct fake_teb *g_teb;

/* image bookkeeping shared with kernel32 (VirtualQuery / VirtualProtect) */
struct img_section { uintptr_t va; size_t size; uint32_t chars; char name[9]; };
struct img_section g_secs[32];
int g_nsecs;
size_t g_image_size;
uint8_t *g_pageprot;     /* Win32 PAGE_* per 4K page of the image */

void log_msg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("[host] ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

/* ---------------------------------------------------------------- tiny executable-stub allocator */
static uint8_t *stub_page; static size_t stub_used, stub_cap;
static pthread_mutex_t stub_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *stub_alloc(size_t n) {
    pthread_mutex_lock(&stub_lock);
    if (!stub_page || stub_used + n > stub_cap) {
        stub_cap = 1 << 16;
        stub_page = mmap(NULL, stub_cap, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (stub_page == MAP_FAILED) { perror("mmap stubs"); exit(1); }
        stub_used = 0;
    }
    uint8_t *p = stub_page + stub_used; stub_used += (n + 15) & ~15u;
    pthread_mutex_unlock(&stub_lock);
    return p;
}

/* SysV -> Win64 forwarder for callbacks SDL invokes (timer, thread entry, audio, virtual joystick ...) */
struct rev_ent { void *ms, *thunk; };
static struct rev_ent rev_tab[256]; static int rev_n;
void *rev_thunk(void *ms_fn) {
    if (!ms_fn) return NULL;
    pthread_mutex_lock(&stub_lock);
    for (int i = 0; i < rev_n; i++) if (rev_tab[i].ms == ms_fn) { void *t = rev_tab[i].thunk; pthread_mutex_unlock(&stub_lock); return t; }
    pthread_mutex_unlock(&stub_lock);
    uint8_t *p = stub_alloc(48), *q = p;
    static const uint8_t pre[] = { 0x49,0x89,0xD0, 0x49,0x89,0xC9, 0x48,0x89,0xF9, 0x48,0x89,0xF2, 0x48,0x83,0xEC,0x28, 0x48,0xB8 };
    memcpy(q, pre, sizeof pre); q += sizeof pre;
    memcpy(q, &ms_fn, 8); q += 8;
    static const uint8_t post[] = { 0xFF,0xD0, 0x48,0x83,0xC4,0x28, 0xC3 };
    memcpy(q, post, sizeof post);
    pthread_mutex_lock(&stub_lock);
    if (rev_n < 256) { rev_tab[rev_n].ms = ms_fn; rev_tab[rev_n].thunk = p; rev_n++; }
    pthread_mutex_unlock(&stub_lock);
    return p;
}

static MS void trap_import(const char *name) {
    fprintf(stderr, "\n[host] FATAL: the game called an import that is not implemented yet: %s\n", name);
    fflush(stderr);
    _exit(70);
}
static void *make_trap(const char *name) {
    uint8_t *p = stub_alloc(40), *q = p;
    *q++ = 0x48; *q++ = 0xB9; memcpy(q, &name, 8); q += 8;             /* mov rcx, name */
    void *h = (void *)trap_import;
    *q++ = 0x48; *q++ = 0xB8; memcpy(q, &h, 8); q += 8;                /* mov rax, trap */
    *q++ = 0x48; *q++ = 0x83; *q++ = 0xEC; *q++ = 0x28;                /* sub rsp, 0x28 */
    *q++ = 0xFF; *q++ = 0xD0;                                          /* call rax */
    return p;
}

/* SCUD_HOST_TRACE=2: wrap every function import in a stub that logs its name, then jumps to the real one */
#include <stdatomic.h>
static struct { const char *name; _Atomic long n, last; } g_cnt[1024];
static void cnt_dump(void) {
    static _Atomic int busy; if (atomic_exchange(&busy, 1)) return;
    fprintf(stderr, "[calls/5s]");
    for (int i = 0; i < 1024 && g_cnt[i].name; i++) { long n = g_cnt[i].n, d = n - g_cnt[i].last; g_cnt[i].last = n; if (d > 200) fprintf(stderr, " %s=%ld", g_cnt[i].name, d); }
    fprintf(stderr, "\n"); atomic_store(&busy, 0);
}
void trace_call(const char *name) {
    if (g_trace == 3) {
        static _Atomic long t0; long now = (long)(clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000000000ull);
        for (int i = 0; i < 1024; i++) {
            if (g_cnt[i].name == name) { g_cnt[i].n++; break; }
            if (!g_cnt[i].name) { g_cnt[i].name = name; g_cnt[i].n = 1; break; }
        }
        long o = t0; if (now - o >= 5 && atomic_compare_exchange_strong(&t0, &o, now)) cnt_dump();
        return;
    }
    char b[160]; int n = snprintf(b, sizeof b, "[call] %s\n", name); write(2, b, n);
}
extern void tracer_common(void);
static void *make_tracer(const char *name, void *target) {
    uint8_t *p = stub_alloc(40), *q = p; void *tc = (void *)tracer_common;
    *q++ = 0x49; *q++ = 0xBB; memcpy(q, &name, 8); q += 8;        /* mov r11, name   */
    *q++ = 0x49; *q++ = 0xBA; memcpy(q, &target, 8); q += 8;      /* mov r10, target */
    *q++ = 0x48; *q++ = 0xB8; memcpy(q, &tc, 8); q += 8;          /* mov rax, tracer_common */
    *q++ = 0xFF; *q++ = 0xE0;                                      /* jmp rax */
    return p;
}
static int is_data_import(const char *n) { return !strcmp(n, "_commode") || !strcmp(n, "_fmode") || !strcmp(n, "_environ") || !strcmp(n, "__initenv"); }

void *host_lookup(const char *name);
static void *lookup_import(const char *name) {
    static const struct import_entry *tabs[] = { k32_table, crt_table, misc_table, sdl_special_table, sdl_generated_table };
    for (size_t t = 0; t < sizeof tabs / sizeof *tabs; t++)
        for (const struct import_entry *e = tabs[t]; e->name; e++)
            if (!strcmp(e->name, name)) return e->fn;
    return NULL;
}

void *host_lookup(const char *name) { return lookup_import(name); }

/* ---------------------------------------------------------------- PE structures (just what we need) */
#pragma pack(push, 1)
struct dos_hdr { uint16_t e_magic; uint8_t pad[58]; int32_t e_lfanew; };
struct file_hdr { uint16_t machine, nsec; uint32_t ts, symtab, nsym; uint16_t optsz, chars; };
struct data_dir { uint32_t rva, size; };
struct opt_hdr64 {
    uint16_t magic; uint8_t lmaj, lmin; uint32_t szcode, szinit, szuninit, entry, basecode;
    uint64_t imagebase; uint32_t secalign, filealign; uint16_t osmaj, osmin, imaj, imin, smaj, smin;
    uint32_t win32ver, sizeimage, sizehdrs, checksum; uint16_t subsys, dllchars;
    uint64_t stkres, stkcommit, heapres, heapcommit; uint32_t loaderflags, ndirs; struct data_dir dir[16];
};
struct sec_hdr { char name[8]; uint32_t vsize, va, rawsize, rawptr, reloc, line; uint16_t nreloc, nline; uint32_t chars; };
struct imp_desc { uint32_t oft, ts, fwd, name, ft; };
struct reloc_blk { uint32_t va, size; };
#pragma pack(pop)

static uint8_t *read_file(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY); if (fd < 0) return NULL;
    struct stat st; fstat(fd, &st);
    uint8_t *b = malloc(st.st_size); size_t got = 0;
    while (got < (size_t)st.st_size) { ssize_t r = read(fd, b + got, st.st_size - got); if (r <= 0) break; got += r; }
    close(fd); *len = got; return b;
}

static int prot_for(uint32_t ch) {
    int p = 0;
    if (ch & 0x40000000) p |= PROT_READ;
    if (ch & 0x80000000) p |= PROT_WRITE;
    if (ch & 0x20000000) p |= PROT_READ | PROT_EXEC;
    return p ? p : PROT_READ;
}
static uint8_t win_prot_for(uint32_t ch) {
    int x = !!(ch & 0x20000000), w = !!(ch & 0x80000000);
    return x ? (w ? 0x40 : 0x20) : (w ? 0x04 : 0x02);
}

static uint64_t *g_gs20_cell;   /* pointer cell within +-2 GB of the image, holds &gs20_stub */
extern void gs20_stub(void);
/* The game statically links a libm whose log2/atan2/fmod/... use x87 (fyl2x, fpatan, fprem, frndint), which Rosetta 2 emulates ~50x slower than SSE.
   Overwrite those entry points with jumps to SSE/libm versions (they sit at fixed addresses in the 0x140000000 image). */
#include <math.h>
#define XF(ret, name, ...) static MS ret x87_##name(__VA_ARGS__)
XF(float, log2f, float x) { return (float)log2((double)x); }
XF(double, log2, double x) { return log2(x); }
XF(float, nearbyintf, float x) { return nearbyintf(x); }
XF(double, nearbyint, double x) { return nearbyint(x); }
XF(float, acosf, float x) { return acosf(x); }
XF(float, asinf, float x) { return asinf(x); }
XF(float, atan2f, float y, float x) { return atan2f(y, x); }
XF(double, atan2, double y, double x) { return atan2(y, x); }
XF(float, fmodf, float x, float y) { return fmodf(x, y); }
XF(double, fmod, double x, double y) { return fmod(x, y); }
/* The game's scud_public_env_lock (start of main) deletes every M3_* environment variable, so its debug/tuning switches are unreachable.
   Opt-in only (SCUD_HOST_KEEP_ENV=1, for offline tuning): make it return immediately. Never set it for online play. */
static void patch_env_lock(uint8_t *base) {
    if (!getenv("SCUD_HOST_KEEP_ENV") || (uint64_t)base != 0x140000000ull) return;
    base[0x140172ee0ull - 0x140000000ull] = 0xC3;
    log_msg("SCUD_HOST_KEEP_ENV: the game's M3_* environment lock is disabled");
}
static void patch_x87(uint8_t *base) {
    static const struct { uint64_t va; void *fn; } t[] = {
        { 0x140339b30, x87_log2f }, { 0x140339ac0, x87_log2 }, { 0x140339bd0, x87_nearbyintf }, { 0x140339b90, x87_nearbyint },
        { 0x140339000, x87_acosf }, { 0x140339030, x87_asinf }, { 0x140339060, x87_atan2f }, { 0x140339090, x87_atan2 },
        { 0x140339880, x87_fmodf }, { 0x140339850, x87_fmod },
    };
    if (!getenv("SCUD_HOST_SSE_LIBM")) return;                  /* only useful under Rosetta (macOS); native x87 is fast, so off by default here */
    if ((uint64_t)base != 0x140000000ull) return;
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) {
        uint8_t *p = base + (t[i].va - 0x140000000ull);
        p[0] = 0x48; p[1] = 0xB8; memcpy(p + 2, &t[i].fn, 8); p[10] = 0xFF; p[11] = 0xE0;   /* mov rax, fn; jmp rax */
    }
}
static int patch_gs(uint8_t *text, size_t len) {
    int n = 0;
    for (size_t i = 0; i + 9 <= len; i++) {
        /* mov rax, qword ptr gs:[imm32]  =  65 48 8B 04 25 <imm32> */
        if (text[i] == 0x65 && text[i+1] == 0x48 && text[i+2] == 0x8B && text[i+3] == 0x04 && text[i+4] == 0x25) {
            uint32_t off; memcpy(&off, text + i + 5, 4);
            if (off == 0x20 && g_gs20_cell) {                    /* GetCurrentFiber(): per-thread, via a transparent stub */
                int32_t rel = (int32_t)((uintptr_t)g_gs20_cell - (uintptr_t)(text + i + 6));
                text[i] = 0xFF; text[i+1] = 0x15; memcpy(text + i + 2, &rel, 4);   /* call [rip+rel] */
                text[i+6] = 0x0F; text[i+7] = 0x1F; text[i+8] = 0x00;               /* 3-byte nop */
                n++;
            } else if (off == 0x30 || off == 0x08 || off == 0x10) {
                text[i] = 0x90;                                   /* drop the gs: prefix (nop) */
                uint32_t abs = FAKE_TEB_ADDR + off; memcpy(text + i + 5, &abs, 4);
                n++;
            }
        }
    }
    return n;
}

static int load_pe(const char *path, void **entry_out) {
    size_t flen; uint8_t *f = read_file(path, &flen);
    if (!f) { log_msg("cannot read %s: %s", path, strerror(errno)); return -1; }
    struct dos_hdr *dh = (void *)f;
    if (dh->e_magic != 0x5A4D) { log_msg("%s: not a PE (MZ)", path); return -1; }
    uint8_t *nt = f + dh->e_lfanew;
    if (memcmp(nt, "PE\0\0", 4)) { log_msg("%s: bad PE signature", path); return -1; }
    struct file_hdr *fh = (void *)(nt + 4);
    struct opt_hdr64 *oh = (void *)(nt + 4 + sizeof *fh);
    if (fh->machine != 0x8664 || oh->magic != 0x20B) { log_msg("%s: not a PE32+ x86-64 image", path); return -1; }
    struct sec_hdr *sh = (void *)((uint8_t *)oh + fh->optsz);

    size_t size = (oh->sizeimage + 0xFFF) & ~0xFFFul;
    void *want = (void *)(uintptr_t)oh->imagebase;
    uint8_t *base = mmap(want, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (base == MAP_FAILED) { log_msg("cannot map image at %p: %s", want, strerror(errno)); return -1; }
    g_image_base = (uintptr_t)base; g_image_size = size;
    memcpy(base, f, oh->sizehdrs);
    g_pageprot = calloc(size / 4096 + 1, 1);
    { void *cell = mmap(base + size, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
      if (cell != MAP_FAILED) { g_gs20_cell = cell; *g_gs20_cell = (uint64_t)(uintptr_t)gs20_stub; } else log_msg("no room for the GetCurrentFiber stub cell"); }
    memset(g_pageprot, 0x02, size / 4096);
    for (int i = 0; i < fh->nsec; i++) {
        uint32_t vs = sh[i].vsize ? sh[i].vsize : sh[i].rawsize;
        if (sh[i].rawsize) memcpy(base + sh[i].va, f + sh[i].rawptr, sh[i].rawsize < vs ? sh[i].rawsize : vs);
        if (i < 32) {
            g_secs[i].va = sh[i].va; g_secs[i].size = vs; g_secs[i].chars = sh[i].chars;
            memcpy(g_secs[i].name, sh[i].name, 8); g_secs[i].name[8] = 0; g_nsecs = i + 1;
        }
    }

    /* base relocations (only needed if we did not get the preferred base) */
    intptr_t delta = (intptr_t)base - (intptr_t)oh->imagebase;
    if (delta && oh->dir[5].size) {
        uint8_t *r = base + oh->dir[5].rva, *end = r + oh->dir[5].size;
        while (r < end) {
            struct reloc_blk *b = (void *)r; uint16_t *e = (void *)(b + 1);
            for (uint32_t k = 0; k < (b->size - 8) / 2; k++) {
                if ((e[k] >> 12) == 10) *(int64_t *)(base + b->va + (e[k] & 0xFFF)) += delta;
            }
            r += b->size;
        }
    }

    /* imports */
    int missing = 0, total = 0;
    if (oh->dir[1].size) {
        for (struct imp_desc *d = (void *)(base + oh->dir[1].rva); d->name; d++) {
            const char *dll = (const char *)base + d->name;
            uint64_t *ilt = (void *)(base + (d->oft ? d->oft : d->ft)), *iat = (void *)(base + d->ft);
            for (; *ilt; ilt++, iat++) {
                total++;
                if (*ilt >> 63) { *iat = (uint64_t)make_trap(strdup("<ordinal import>")); missing++; continue; }
                const char *name = (const char *)base + (*ilt & 0x7FFFFFFF) + 2;
                void *fn = lookup_import(name);
                if (!fn) { fn = make_trap(strdup(name)); missing++; if (g_trace) log_msg("unimplemented import: %s!%s", dll, name); }
                if (g_trace > 1 && !is_data_import(name)) fn = make_tracer(strdup(name), fn);
                *iat = (uint64_t)fn;
            }
        }
    }
    log_msg("image %s mapped at %p (%zu KB); %d imports, %d still trapped", path, base, size >> 10, total, missing);

    /* section protections + gs: patch */
    for (int i = 0; i < fh->nsec; i++) {
        uint32_t vs = sh[i].vsize ? sh[i].vsize : sh[i].rawsize;
        if (sh[i].chars & 0x20000000) {
            int n = patch_gs(base + sh[i].va, vs);
            log_msg("patched %d gs: TEB reads in %.8s", n, sh[i].name);
        }
    }
    patch_x87(base);
    patch_env_lock(base);
    for (int i = 0; i < fh->nsec; i++) {
        uint32_t vs = sh[i].vsize ? sh[i].vsize : sh[i].rawsize;
        size_t len = (vs + 0xFFF) & ~0xFFFul; if (!len) continue;
        int p = prot_for(sh[i].chars);
        if (strncmp(sh[i].name, ".idata", 6) == 0 || strncmp(sh[i].name, ".CRT", 4) == 0) p = PROT_READ | PROT_WRITE;
        if (mprotect(base + sh[i].va, len, p)) log_msg("mprotect %.8s failed: %s", sh[i].name, strerror(errno));
        memset(g_pageprot + sh[i].va / 4096, win_prot_for(sh[i].chars), len / 4096);
    }
    *entry_out = base + oh->entry;
    free(f);
    return 0;
}

static void setup_teb(void) {
    void *p = mmap((void *)(uintptr_t)FAKE_TEB_ADDR, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) { log_msg("cannot map the fake TEB at %#x: %s (is the pagezero small enough?)", FAKE_TEB_ADDR, strerror(errno)); exit(1); }
    g_teb = p;
    char probe; /* StackBase / StackLimit just have to be plausible and unique for this thread */
    g_teb->tib[1] = ((uintptr_t)&probe + 0x10000) & ~0xFFFFul;
    g_teb->tib[2] = ((uintptr_t)&probe - 0x700000) & ~0xFFFFul;
    g_teb->tib[6] = FAKE_TEB_ADDR;      /* NT_TIB.Self at +0x30 */
}

/* SCUD_HOST_PROF=<file>: CPU-time sampling profiler (guest instruction pointers; post-process against the disassembly) */
#define PROF_MAX (1 << 21)
static uint64_t prof_buf[PROF_MAX]; static volatile uint32_t prof_n; static const char *prof_file;
static void prof_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si; ucontext_t *u = uc; uint32_t i = __atomic_fetch_add(&prof_n, 1, __ATOMIC_RELAXED);
    if (i < PROF_MAX) prof_buf[i] = u->uc_mcontext.gregs[REG_RIP];
}
static void prof_write(void) {
    FILE *f = fopen(prof_file, "w"); uint32_t n = prof_n < PROF_MAX ? prof_n : PROF_MAX;
    if (f) { for (uint32_t i = 0; i < n; i++) {
        Dl_info di; uint64_t a = prof_buf[i];
        if ((a < 0x140000000ull || a >= 0x141500000ull) && dladdr((void *)a, &di) && di.dli_sname) fprintf(f, "%llx %s\n", (unsigned long long)a, di.dli_sname);
        else fprintf(f, "%llx\n", (unsigned long long)a);
    } fclose(f); }
}
static void prof_dump(int sig) { (void)sig; prof_write(); _exit(0); }
static void prof_start(void) {
    prof_file = getenv("SCUD_HOST_PROF"); if (!prof_file) return;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = prof_handler; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigaction(SIGPROF, &sa, NULL);
    signal(SIGTERM, prof_dump);
    struct itimerval it = { { 0, 1000 }, { 0, 1000 } }; setitimer(ITIMER_PROF, &it, NULL);
}
static void crash_handler(int sig, siginfo_t *si, void *uc) {
    ucontext_t *u = uc; greg_t *g = u->uc_mcontext.gregs;
    struct { uint64_t __rax, __rbx, __rcx, __rdx, __rsi, __rdi, __rbp, __rsp, __r8, __r9, __r10, __r11, __r12, __r13, __r14, __r15, __rip; } rs = {
        g[REG_RAX], g[REG_RBX], g[REG_RCX], g[REG_RDX], g[REG_RSI], g[REG_RDI], g[REG_RBP], g[REG_RSP], g[REG_R8], g[REG_R9], g[REG_R10], g[REG_R11],
        g[REG_R12], g[REG_R13], g[REG_R14], g[REG_R15], g[REG_RIP] }, *r = &rs;
    char b[1024]; int n = snprintf(b, sizeof b, "\n[host] CRASH signal %d at rip=%#llx (image+%#llx) fault addr=%p\n"
        "  rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx rbp=%#llx rsp=%#llx\n  r8=%#llx r9=%#llx r10=%#llx r11=%#llx r12=%#llx r13=%#llx r14=%#llx r15=%#llx\n  stack:",
        sig, (unsigned long long)r->__rip, (unsigned long long)(r->__rip - g_image_base), si->si_addr,
        r->__rax, r->__rbx, r->__rcx, r->__rdx, r->__rsi, r->__rdi, r->__rbp, r->__rsp, r->__r8, r->__r9, r->__r10, r->__r11, r->__r12, r->__r13, r->__r14, r->__r15);
    uint64_t *sp = (uint64_t *)r->__rsp;
    for (int i = 0; i < 12 && n < (int)sizeof b - 24; i++) n += snprintf(b + n, sizeof b - n, " %#llx", (unsigned long long)sp[i]);
    { Dl_info di; if (dladdr((void *)r->__rip, &di)) n += snprintf(b + n, sizeof b - n - 2, "\n  in %s`%s+%#llx", di.dli_fname ? strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname : "?", di.dli_sname ? di.dli_sname : "?", (unsigned long long)((char *)r->__rip - (char *)di.dli_saddr)); }
    b[n++] = '\n'; write(2, b, n); if (prof_file) prof_write(); _exit(139);
}
static void at_exit_log(void) { log_msg("process exiting"); }
static int self_exe(char *buf, size_t cap) { ssize_t n = readlink("/proc/self/exe", buf, cap - 1); if (n <= 0) return -1; buf[n] = 0; return 0; }
static void find_dirs(const char *argv0) {
    const char *env = getenv("SCUDRACE_HOME");
    char exe[2048];
    if (env && *env) { snprintf(g_game_dir, sizeof g_game_dir, "%s", env); }
    else if (self_exe(exe, sizeof exe) == 0) {
        char real[PATH_MAX]; if (!realpath(exe, real)) snprintf(real, sizeof real, "%s", exe);
        char *dir = dirname(real);                       /* .../Foo.app/Contents/MacOS or a plain dir */
        char *app = strstr(dir, ".app/Contents/MacOS");
        if (app) { char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%.*s", (int)(app - dir), dir); snprintf(g_game_dir, sizeof g_game_dir, "%s", dirname(tmp)); }
        else snprintf(g_game_dir, sizeof g_game_dir, "%s", dir);
    } else getcwd(g_game_dir, sizeof g_game_dir);
    if (!(env && *env)) {                                /* self-contained layout: <folder>/game/scudrace.exe next to <folder>/ScudRace.app and <folder>/roms/ */
        char t[PATH_MAX + 32]; snprintf(t, sizeof t, "%s/game/scudrace.exe", g_game_dir);
        if (!access(t, R_OK)) { size_t l = strlen(g_game_dir); snprintf(g_game_dir + l, sizeof g_game_dir - l, "/game"); return; }
        snprintf(t, sizeof t, "%s/../game/scudrace.exe", g_game_dir);          /* running straight from build/ */
        char up[PATH_MAX]; if (!access(t, R_OK) && realpath(dirname(t), up)) { snprintf(g_game_dir, sizeof g_game_dir, "%s", up); return; }
    }
    if (!(env && *env)) {                                /* the .app may live in a subfolder (native-mac/) of the game folder */
        char t[PATH_MAX + 32], up[PATH_MAX]; snprintf(t, sizeof t, "%s/scudrace.exe", g_game_dir);
        if (access(t, R_OK)) { snprintf(t, sizeof t, "%s/../scudrace.exe", g_game_dir); if (!access(t, R_OK) && realpath(dirname(t), up)) snprintf(g_game_dir, sizeof g_game_dir, "%s", up); }
    }
    (void)argv0;
}

int main(int argc, char **argv) {
    g_trace = getenv("SCUD_HOST_TRACE") ? atoi(getenv("SCUD_HOST_TRACE")) : 0; if (getenv("SCUD_HOST_TRACE") && g_trace < 1) g_trace = 1;
        /* Output goes through SDL's OpenGL renderer by default (same backend on macOS and Linux); SCUD_RENDER=metal|opengl|software overrides. */
    { const char *r = getenv("SCUD_RENDER"); setenv("SDL_RENDER_DRIVER", r && *r ? r : "opengl", 1); }
    find_dirs(argv[0]);
    if (chdir(g_game_dir)) { log_msg("cannot chdir to %s: %s", g_game_dir, strerror(errno)); return 1; }

    char exe[2048];
    const char *e = getenv("SCUDRACE_EXE");
    if (e) snprintf(exe, sizeof exe, "%s", e);
    else {
        snprintf(exe, sizeof exe, "%s/scudrace.exe", g_game_dir);
    }
    /* Windows-style path the game sees: Z:\Users\...\scudrace.exe */
    char winp[1100]; snprintf(winp, sizeof winp, "Z:%s/scudrace.exe", g_game_dir);
    for (char *c = winp; *c; c++) if (*c == '/') *c = '\\';
    snprintf(g_exe_path_win, sizeof g_exe_path_win, "%s", winp);

    log_msg("game folder: %s", g_game_dir);
    setup_teb();
    extern void host_set_args(int, char **);
    { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
      static char altstack[1 << 16]; stack_t ss = { altstack, sizeof altstack, 0 }; sigaltstack(&ss, NULL);
      sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGFPE, &sa, NULL); sigaction(SIGABRT, &sa, NULL); }
    atexit(at_exit_log);
    prof_start();
    { extern void host_stats_start(void); host_stats_start(); }
    if (argc > 1 && !strcmp(argv[1], "--selftest")) { extern int host_selftest(void); host_set_args(argc, argv); return host_selftest(); }
    void *entry;
    if (load_pe(exe, &entry)) return 1;
    extern void host_set_args(int argc, char **argv);
    host_set_args(argc, argv);
    log_msg("entering the game at %p", entry);
    int rc = ((MS int (*)(void))entry)();
    log_msg("the game returned %d", rc);
    return rc;
}
