/* Runtime for the lifted (statically recompiled) game code.
 *
 * Every function of the original x86-64 image was translated ahead of time into a C function f_<addr>(R) that works on
 * an x86-64 register file R (Ghidra register-space layout) and on guest memory at its real addresses. This file supplies
 * what the translated code needs at run time: indirect calls, the Windows imports (called through typed thunks that
 * move the Win64 arguments into native C arguments), calls from the host back into guest code, per-thread register
 * files / guest stacks / TEBs, setjmp/longjmp and fibers.
 */
#include "common.h"
#include "../lift/lift_rt.h"
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#define O_RAX 0x00
#define O_RCX 0x08
#define O_RDX 0x10
#define O_RBX 0x18
#define O_RSP 0x20
#define O_RBP 0x28
#define O_RSI 0x30
#define O_RDI 0x38
#define O_R8  0x80
#define O_R9  0x88
#define O_R12 0xa0
#define O_GS  0x118
#define O_XMM(n) (0x1200 + 0x40 * (n))
#define REGFILE_SIZE 0x4000

static inline u64 rg(uint8_t *R, int o) { u64 v; memcpy(&v, R + o, 8); return v; }
static inline void wg(uint8_t *R, int o, u64 v) { memcpy(R + o, &v, 8); }

/* ------------------------------------------------------------------ per-thread guest state */
struct gthread { uint8_t *R; uint8_t *stack; size_t stack_size; uint64_t *teb; };
static _Thread_local struct gthread *t_g;
_Thread_local void *rt_cur_jb;

static struct gthread *gthread_new(size_t stack) {
    struct gthread *g = calloc(1, sizeof *g);
    if (posix_memalign((void **)&g->R, 64, REGFILE_SIZE)) abort();
    memset(g->R, 0, REGFILE_SIZE);
    if (stack < (1u << 20)) stack = 1u << 20;
    stack = (stack + 0xFFFF) & ~(size_t)0xFFFF;
    g->stack = mmap(NULL, stack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g->stack == MAP_FAILED) { perror("guest stack"); abort(); }
    g->stack_size = stack;
    g->teb = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint64_t top = (uint64_t)(uintptr_t)(g->stack + stack - 0x100) & ~0xFull;
    g->teb[1] = top;                             /* NT_TIB.StackBase */
    g->teb[2] = (uint64_t)(uintptr_t)g->stack;   /* NT_TIB.StackLimit */
    g->teb[6] = (uint64_t)(uintptr_t)g->teb;     /* NT_TIB.Self (gs:[0x30]) */
    wg(g->R, O_RSP, top);
    wg(g->R, O_GS, (u64)(uintptr_t)g->teb);
    return g;
}
static struct gthread *cur(void) { if (!t_g) t_g = gthread_new(1u << 20); return t_g; }
uint8_t *rt_regs(void) { return cur()->R; }
uint64_t *rt_teb(void) { return cur()->teb; }

/* ------------------------------------------------------------------ imports */
#define TOK_BASE 0x00007ff000000000ull
#define TOK_MAX 4096
struct imp {
    const char *name; void *fn;
    uint8_t nargs, ret, variadic; char kind[16];
};
static struct imp g_imps[TOK_MAX]; static int g_nimps;

static int sig_ok(const char *s) {
    int ni = 0, nf = 0;
    for (const char *p = s + 2; *p; p++) { if (*p == 'f' || *p == 'd') nf++; else if (*p != 'V') ni++; }
    return ni <= 12 && nf <= 8 && strlen(s + 2) <= 16;   /* integer args 9-12 go on the native stack: shims must declare them 64-bit */
}
/* a token the guest can call: an address that is never mapped, decoded by guest_call */
uint64_t rt_import_token(const char *name, void *fn, const char *sig) {
    if (g_nimps >= TOK_MAX) { log_msg("too many imports"); abort(); }
    struct imp *m = &g_imps[g_nimps];
    m->name = name; m->fn = fn;
    if (fn && sig) {
        if (!sig_ok(sig)) { log_msg("import %s: signature %s does not fit the thunk", name, sig); m->fn = NULL; }
        m->ret = (uint8_t)sig[0];
        m->variadic = strchr(sig, 'V') != NULL;
        const char *a = sig + 2; m->nargs = (uint8_t)strlen(a); memcpy(m->kind, a, m->nargs);
    }
    return TOK_BASE + 16ull * (uint64_t)g_nimps++;
}

typedef u64 (*fn_i)(u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double, double);
typedef double (*fn_d)(u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double, double);
typedef float (*fn_f)(u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double, double);

static void call_import(uint8_t *R, struct imp *m) {
    u64 rsp = rg(R, O_RSP);
    if (!m->fn) {
        fprintf(stderr, "\n[host] FATAL: the game called an import that is not implemented: %s\n", m->name); fflush(stderr); _exit(70);
    }
    u64 iv[12] = { 0 }; double dv[8] = { 0 }; int ni = 0, nf = 0;
    if (m->variadic) {                       /* the shim takes a pointer to the Win64 argument slots */
        static _Thread_local u64 slots[20];
        slots[0] = rg(R, O_RCX); slots[1] = rg(R, O_RDX); slots[2] = rg(R, O_R8); slots[3] = rg(R, O_R9);
        memcpy(&slots[4], (void *)(uintptr_t)(rsp + 0x28), 16 * 8);
        iv[0] = (u64)(uintptr_t)slots; ni = 1;
    } else {
        static const int ireg[4] = { O_RCX, O_RDX, O_R8, O_R9 };
        for (int i = 0; i < m->nargs; i++) {
            char k = m->kind[i]; u64 raw;
            if (i < 4) raw = (k == 'f' || k == 'd') ? rg(R, O_XMM(i)) : rg(R, ireg[i]);
            else memcpy(&raw, (void *)(uintptr_t)(rsp + 0x28 + 8 * (i - 4)), 8);
            switch (k) {
            case 'f': { double d = 0; uint32_t b = (uint32_t)raw; memcpy(&d, &b, 4); dv[nf++] = d; break; }   /* low lane = the float */
            case 'd': { double d; memcpy(&d, &raw, 8); dv[nf++] = d; break; }
            case 'w': iv[ni++] = (uint32_t)raw; break;
            case 'h': iv[ni++] = (uint16_t)raw; break;
            case 'b': iv[ni++] = (uint8_t)raw; break;
            default:  iv[ni++] = raw; break;
            }
        }
    }
#define ARGS iv[0], iv[1], iv[2], iv[3], iv[4], iv[5], iv[6], iv[7], iv[8], iv[9], iv[10], iv[11], dv[0], dv[1], dv[2], dv[3], dv[4], dv[5], dv[6], dv[7]
    switch (m->ret) {
    case 'd': { double r = ((fn_d)m->fn)(ARGS); memset(R + O_XMM(0), 0, 16); memcpy(R + O_XMM(0), &r, 8); break; }
    case 'f': { float r = ((fn_f)m->fn)(ARGS); memset(R + O_XMM(0), 0, 16); memcpy(R + O_XMM(0), &r, 4); break; }
    case 'v': ((fn_i)m->fn)(ARGS); break;
    case 'w': wg(R, O_RAX, (uint32_t)((fn_i)m->fn)(ARGS)); break;
    case 'h': wg(R, O_RAX, (uint16_t)((fn_i)m->fn)(ARGS)); break;
    case 'b': wg(R, O_RAX, (uint8_t)((fn_i)m->fn)(ARGS)); break;
    default:  wg(R, O_RAX, ((fn_i)m->fn)(ARGS)); break;
    }
#undef ARGS
    wg(R, O_RSP, rg(R, O_RSP) + 8);          /* the "ret" back to the guest caller */
}

/* ------------------------------------------------------------------ indirect calls */
#define CACHE_N 8192
static struct { u64 addr; void (*fn)(uint8_t *restrict); } g_cache[CACHE_N];
static void (*lookup_lifted(u64 a))(uint8_t *restrict) {
    unsigned h = (unsigned)((a >> 2) ^ (a >> 15)) & (CACHE_N - 1);
    if (g_cache[h].addr == a) return g_cache[h].fn;
    unsigned lo = 0, hi = lift_table_n;
    while (lo < hi) { unsigned mid = (lo + hi) / 2; if (lift_table[mid].addr < a) lo = mid + 1; else hi = mid; }
    if (lo < lift_table_n && lift_table[lo].addr == a) {
        g_cache[h].fn = lift_table[lo].fn; __atomic_store_n(&g_cache[h].addr, a, __ATOMIC_RELEASE);   /* racy but each entry is self-consistent enough: fn written first */
        return lift_table[lo].fn;
    }
    return NULL;
}
void guest_call(uint8_t *R, u64 addr) {
    if (addr - TOK_BASE < 16ull * TOK_MAX) { call_import(R, &g_imps[(addr - TOK_BASE) >> 4]); return; }
    void (*f)(uint8_t *restrict) = lookup_lifted(addr);
    if (f) { f(R); return; }
    fprintf(stderr, "\n[host] FATAL: call/jump to %#llx, which is not the start of a translated function (rsp=%#llx, [rsp]=%#llx)\n",
            (unsigned long long)addr, (unsigned long long)rg(R, O_RSP), (unsigned long long)LD(u64, rg(R, O_RSP)));
    fflush(stderr); _exit(71);
}
void guest_jump(uint8_t *R, u64 addr) { guest_call(R, addr); }

/* host -> guest: run a guest function (Win64 convention, up to 4 integer args) on this thread's register file */
uint64_t call_guest(uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint8_t *R = cur()->R;
    u64 sp = rg(R, O_RSP);
    u64 nsp = ((sp - 0x40) & ~0xFull) - 8;    /* [nsp] = return address, nsp+8 .. +0x28 = shadow space; entry rsp % 16 == 8 */
    ST(u64, nsp, 0x00007fefdead0000ull);
    wg(R, O_RSP, nsp); wg(R, O_RCX, a0); wg(R, O_RDX, a1); wg(R, O_R8, a2); wg(R, O_R9, a3);
    guest_call(R, fn);
    wg(R, O_RSP, sp);
    return rg(R, O_RAX);
}

/* guest function pointer -> native function pointer (SDL callbacks, thread entries): a pool of trampolines */
#define NCB 64
static u64 g_cb[NCB]; static int g_ncb; static pthread_mutex_t g_cb_lock = PTHREAD_MUTEX_INITIALIZER;
#define CB(k, j) static u64 cb_##k##j(u64 a, u64 b, u64 c, u64 d) { return call_guest(g_cb[k * 8 + j], a, b, c, d); }
#define CB8(k) CB(k, 0) CB(k, 1) CB(k, 2) CB(k, 3) CB(k, 4) CB(k, 5) CB(k, 6) CB(k, 7)
CB8(0) CB8(1) CB8(2) CB8(3) CB8(4) CB8(5) CB8(6) CB8(7)
#define CBP(k) cb_##k
#define CBP8(k) CBP(k##0), CBP(k##1), CBP(k##2), CBP(k##3), CBP(k##4), CBP(k##5), CBP(k##6), CBP(k##7)
static u64 (*const g_cbfn[NCB])(u64, u64, u64, u64) = { CBP8(0), CBP8(1), CBP8(2), CBP8(3), CBP8(4), CBP8(5), CBP8(6), CBP8(7) };
void *rev_thunk(void *guest_fn) {
    if (!guest_fn) return NULL;
    u64 a = (u64)(uintptr_t)guest_fn;
    pthread_mutex_lock(&g_cb_lock);
    int k = 0; while (k < g_ncb && g_cb[k] != a) k++;
    if (k == g_ncb) { if (g_ncb == NCB) { log_msg("out of callback trampolines"); abort(); } g_cb[g_ncb++] = a; }
    pthread_mutex_unlock(&g_cb_lock);
    return (void *)g_cbfn[k];
}

/* ------------------------------------------------------------------ misc hooks used by the translated code */
static int g_biglock;
void host_lock(void) { while (__atomic_exchange_n(&g_biglock, 1, __ATOMIC_ACQUIRE)) while (__atomic_load_n(&g_biglock, __ATOMIC_RELAXED)) ; }
void host_unlock(void) { __atomic_store_n(&g_biglock, 0, __ATOMIC_RELEASE); }
u64 host_rdtsc(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec; }
void lift_fatal(const char *what) {
    uint8_t *R = t_g ? t_g->R : NULL;
    fprintf(stderr, "\n[host] FATAL in translated code: %s (rsp=%#llx)\n", what, R ? (unsigned long long)rg(R, O_RSP) : 0ull);
    fflush(stderr); _exit(72);
}

/* ------------------------------------------------------------------ setjmp / longjmp
 * The translated caller of _setjmp runs a real C setjmp in its own frame (see gen_lift.py) and leaves the jmp_buf in
 * rt_cur_jb; the guest jmp_buf keeps that pointer plus the guest's callee-saved registers. */
static const int g_saved[] = { O_RBX, O_RBP, O_RDI, O_RSI, O_R12, O_R12 + 8, O_R12 + 16, O_R12 + 24 };
static u64 rt_setjmp(u64 *buf, u64 frame) {
    (void)frame; uint8_t *R = cur()->R;
    buf[0] = (u64)(uintptr_t)rt_cur_jb; rt_cur_jb = NULL;
    buf[1] = rg(R, O_RSP) + 8;               /* rsp once _setjmp has returned */
    for (int i = 0; i < 8; i++) buf[2 + i] = rg(R, g_saved[i]);
    for (int i = 0; i < 10; i++) memcpy(&buf[10 + 2 * i], R + O_XMM(6 + i), 16);
    if (!buf[0]) { log_msg("_setjmp called from a place the translator did not prepare"); abort(); }
    return 0;
}
static void rt_longjmp(u64 *buf, int val) {
    uint8_t *R = cur()->R;
    wg(R, O_RSP, buf[1]);
    for (int i = 0; i < 8; i++) wg(R, g_saved[i], buf[2 + i]);
    for (int i = 0; i < 10; i++) memcpy(R + O_XMM(6 + i), &buf[10 + 2 * i], 16);
    wg(R, O_RAX, (uint32_t)(val ? val : 1));
    _longjmp(*(jmp_buf *)(uintptr_t)buf[0], 1);
}

/* fibers (fibers.c) */
extern void *rt_ConvertThreadToFiberEx(void *param, DWORD flags);
extern void *rt_CreateFiberEx(size_t commit, size_t reserve, DWORD flags, u64 fn, void *param);
extern void rt_SwitchToFiber(void *to);
extern void rt_DeleteFiber(void *f);
extern BOOL rt_IsThreadAFiber(void);

const struct import_entry rt_table[] = {
    { "_setjmp", (void *)rt_setjmp, "w:qq" },
    { "longjmp", (void *)rt_longjmp, "v:qw" },
    { "ConvertThreadToFiberEx", (void *)rt_ConvertThreadToFiberEx, "q:qw" },
    { "CreateFiberEx", (void *)rt_CreateFiberEx, "q:qqwqq" },
    { "SwitchToFiber", (void *)rt_SwitchToFiber, "v:q" },
    { "DeleteFiber", (void *)rt_DeleteFiber, "v:q" },
    { "IsThreadAFiber", (void *)rt_IsThreadAFiber, "w:" },
    { 0, 0, 0 }
};

/* fibers.c needs these */
void rt_save_callee(uint8_t *R, uint8_t *save) {
    wg(save, 0, rg(R, O_RSP));
    for (int i = 0; i < 8; i++) wg(save, 8 + 8 * i, rg(R, g_saved[i]));
    for (int i = 0; i < 10; i++) memcpy(save + 72 + 16 * i, R + O_XMM(6 + i), 16);
}
void rt_restore_callee(uint8_t *R, const uint8_t *save) {
    wg(R, O_RSP, rg((uint8_t *)save, 0));
    for (int i = 0; i < 8; i++) wg(R, g_saved[i], rg((uint8_t *)save, 8 + 8 * i));
    for (int i = 0; i < 10; i++) memcpy(R + O_XMM(6 + i), save + 72 + 16 * i, 16);
}
void rt_set_rsp(u64 v) { wg(cur()->R, O_RSP, v); }
