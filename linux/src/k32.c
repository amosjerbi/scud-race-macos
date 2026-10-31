/* kernel32 on macOS: only what scudrace.exe imports. */
#include "common.h"
#include <pthread.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <fnmatch.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define INFINITE 0xFFFFFFFFu
#define WAIT_TIMEOUT 0x102

/* SCUD_HOST_STATS=1: calls per second of the synchronisation / timing primitives */
enum { ST_WAKE, ST_WAKEALL, ST_SLEEPCV, ST_SLEEP, ST_SLEEP0, ST_QPC, ST_SWITCH, ST_SRWACQ, ST_CSENTER, ST_N };
static _Atomic uint64_t g_st[ST_N]; static int g_stats_on;
#define ST(i) do { if (g_stats_on) atomic_fetch_add_explicit(&g_st[i], 1, memory_order_relaxed); } while (0)
static void *stats_thread(void *p) {
    static const char *nm[] = { "WakeCV", "WakeAllCV", "SleepCV", "Sleep(>0)", "Sleep(0)", "QPC", "SwitchToFiber", "SRWAcquire", "CSEnter" };
    (void)p; for (;;) { sleep(1); char b[400]; int n = snprintf(b, sizeof b, "[stats]"); for (int i = 0; i < ST_N; i++) n += snprintf(b + n, sizeof b - n, " %s=%llu", nm[i], (unsigned long long)atomic_exchange(&g_st[i], 0)); fprintf(stderr, "%s\n", b); }
    return NULL;
}
void host_stats_start(void) { if (!getenv("SCUD_HOST_STATS")) return; g_stats_on = 1; pthread_t t; pthread_create(&t, NULL, stats_thread, NULL); pthread_detach(t); }
static _Thread_local DWORD g_lasterr;
static void seterr(DWORD e) { g_lasterr = e; }
static DWORD errno_to_win(int e) {
    switch (e) { case 0: return 0; case ENOENT: return 2; case ENOTDIR: return 3; case EACCES: case EPERM: return 5;
                 case EEXIST: return 80; case ENOMEM: return 8; case EINVAL: return 87; case ENOTEMPTY: return 145; default: return 31; }
}
static MS DWORD GetLastError(void) { return g_lasterr; }
static MS void SetLastError(DWORD e) { g_lasterr = e; }

/* ------------------------------------------------------------------ strings / paths */
size_t utf16_to_utf8(const WCHAR *w, int wlen, char *out, size_t cap) {
    size_t n = 0;
    for (int i = 0; wlen < 0 ? w[i] : i < wlen; i++) {
        uint32_t c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && (wlen < 0 ? w[i + 1] : i + 1 < wlen) && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00); i++;
        }
        uint8_t b[4]; int k;
        if (c < 0x80) { b[0] = c; k = 1; }
        else if (c < 0x800) { b[0] = 0xC0 | (c >> 6); b[1] = 0x80 | (c & 63); k = 2; }
        else if (c < 0x10000) { b[0] = 0xE0 | (c >> 12); b[1] = 0x80 | ((c >> 6) & 63); b[2] = 0x80 | (c & 63); k = 3; }
        else { b[0] = 0xF0 | (c >> 18); b[1] = 0x80 | ((c >> 12) & 63); b[2] = 0x80 | ((c >> 6) & 63); b[3] = 0x80 | (c & 63); k = 4; }
        if (out) { if (n + k > cap) break; memcpy(out + n, b, k); }
        n += k;
    }
    return n;
}
size_t utf8_to_utf16(const char *s, int slen, WCHAR *out, size_t cap) {
    size_t n = 0; const uint8_t *p = (const uint8_t *)s;
    for (int i = 0; slen < 0 ? p[i] : i < slen;) {
        uint32_t c = p[i]; int k = 1;
        if (c >= 0xF0 && c < 0xF8) { c &= 7; k = 4; } else if (c >= 0xE0) { c &= 15; k = 3; } else if (c >= 0xC0) { c &= 31; k = 2; }
        for (int j = 1; j < k; j++) c = (c << 6) | (p[i + j] & 63);
        i += k;
        if (c >= 0x10000) {
            c -= 0x10000;
            if (out) { if (n + 2 > cap) break; out[n] = 0xD800 + (c >> 10); out[n + 1] = 0xDC00 + (c & 0x3FF); }
            n += 2;
        } else { if (out) { if (n + 1 > cap) break; out[n] = c; } n++; }
    }
    return n;
}
void win_path_to_unix(const char *in, char *out, size_t cap) {
    size_t i = 0, o = 0;
    if (!strncmp(in, "\\\\?\\", 4) || !strncmp(in, "//?/", 4)) i = 4;
    if (((in[i] >= 'A' && in[i] <= 'Z') || (in[i] >= 'a' && in[i] <= 'z')) && in[i + 1] == ':') i += 2;   /* drive letter */
    for (; in[i] && o + 1 < cap; i++) out[o++] = in[i] == '\\' ? '/' : in[i];
    out[o] = 0;
    /* the Windows null device: NUL, NUL:, \\.\NUL (also with a directory in front) */
    const char *base = strrchr(out, '/'); base = base ? base + 1 : out;
    if (!strncasecmp(base, "NUL", 3) && (base[3] == 0 || (base[3] == ':' && base[4] == 0)) && (base == out || !strncmp(out, "./", 2) || !strncmp(out, "/./", 3) || !strcmp(out, "//./NUL")))
        snprintf(out, cap, "/dev/null");
    /* the ROM set lives in native-mac/roms/: a lookup of scud.zip that misses in the game folder is served from there */
    else if (!strcmp(base, "scud.zip") && access(out, R_OK)) {
        static const char *const where[] = { "%s/../roms/scud.zip", "%s/roms/scud.zip", "%s/native-mac/roms/scud.zip" };
        for (int k = 0; k < 3; k++) {
            char alt[2048]; snprintf(alt, sizeof alt, where[k], g_game_dir);
            if (!access(alt, R_OK)) { snprintf(out, cap, "%s", alt); break; }
        }
    }
}
void win_wpath_to_unix(const WCHAR *in, char *out, size_t cap) {
    char tmp[2048]; size_t n = utf16_to_utf8(in, -1, tmp, sizeof tmp - 1); tmp[n] = 0;
    win_path_to_unix(tmp, out, cap);
}

static MS int MultiByteToWideChar(DWORD cp, DWORD fl, const char *src, int slen, WCHAR *dst, int dlen) {
    (void)cp; (void)fl;
    if (slen < 0) slen = (int)strlen(src) + 1;
    size_t need = utf8_to_utf16(src, slen, NULL, 0);
    if (!dlen) return (int)need;
    if ((int)need > dlen) { seterr(122); return 0; }
    return (int)utf8_to_utf16(src, slen, dst, dlen);
}
static MS int WideCharToMultiByte(DWORD cp, DWORD fl, const WCHAR *src, int slen, char *dst, int dlen, const char *def, BOOL *used) {
    (void)cp; (void)fl; (void)def;
    if (used) *used = 0;
    if (slen < 0) { slen = 0; while (src[slen]) slen++; slen++; }
    size_t need = utf16_to_utf8(src, slen, NULL, 0);
    if (!dlen) return (int)need;
    if ((int)need > dlen) { seterr(122); return 0; }
    return (int)utf16_to_utf8(src, slen, dst, dlen);
}
static MS BOOL IsDBCSLeadByteEx(DWORD cp, uint8_t b) { (void)cp; (void)b; return 0; }

/* ------------------------------------------------------------------ time */
static MS BOOL QueryPerformanceFrequency(int64_t *f) { *f = 1000000000LL; return 1; }
static MS BOOL QueryPerformanceCounter(int64_t *c) { ST(ST_QPC); *c = (int64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW); return 1; }
static MS uint64_t GetTickCount64(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000000ull; }
static MS void GetSystemTimeAsFileTime(uint64_t *ft) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    *ft = (uint64_t)ts.tv_sec * 10000000ull + ts.tv_nsec / 100 + 116444736000000000ull;
}
static MS void Sleep(DWORD ms) { ST(ms ? ST_SLEEP : ST_SLEEP0);
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

/* ------------------------------------------------------------------ process / module */
static char *g_cmdline_a; static WCHAR g_cmdline_w[2048]; static int s_argc; static char **s_argv;
void host_get_args(int *argc, char ***argv) { *argc = s_argc; *argv = s_argv; }
void host_set_args(int argc, char **argv) {
    s_argc = argc; s_argv = argv;
    char buf[4096]; size_t o = snprintf(buf, sizeof buf, "\"%s\"", g_exe_path_win);
    for (int i = 1; i < argc && o < sizeof buf - 64; i++) o += snprintf(buf + o, sizeof buf - o, " %s", argv[i]);
    g_cmdline_a = strdup(buf);
    utf8_to_utf16(buf, -1, g_cmdline_w, 2047);
}
extern uintptr_t g_image_base;
static MS const WCHAR *GetCommandLineW(void) { return g_cmdline_w; }
static MS DWORD GetModuleFileNameA(HANDLE m, char *buf, DWORD n) {
    (void)m; size_t l = strlen(g_exe_path_win);
    if (l >= n) { if (n) { memcpy(buf, g_exe_path_win, n - 1); buf[n - 1] = 0; } seterr(122); return n; }
    memcpy(buf, g_exe_path_win, l + 1); return (DWORD)l;
}
static MS DWORD GetModuleFileNameW(HANDLE m, WCHAR *buf, DWORD n) {
    (void)m; WCHAR tmp[1200]; size_t l = utf8_to_utf16(g_exe_path_win, -1, tmp, 1199);
    if (l >= n) { if (n) { memcpy(buf, tmp, (n - 1) * 2); buf[n - 1] = 0; } seterr(122); return n; }
    memcpy(buf, tmp, l * 2); buf[l] = 0; return (DWORD)l;
}
static HANDLE fake_module(const char *name) {
    if (!name) return (HANDLE)g_image_base;
    static const char *known[] = { "kernel32", "user32", "ntdll", "gdi32", "shell32", "advapi32", "ole32", "winmm", "dwmapi", "shcore", "msvcrt", "ws2_32", "winhttp", 0 };
    char lo[64]; size_t i = 0; for (; name[i] && i < 63; i++) lo[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]; lo[i] = 0;
    for (int k = 0; known[k]; k++) if (!strncmp(lo, known[k], strlen(known[k]))) return (HANDLE)(uintptr_t)(0x7FF000000000ull + 0x10000ull * (k + 1));
    return NULL;
}
static MS HANDLE GetModuleHandleA(const char *n) { return fake_module(n); }
static MS HANDLE GetModuleHandleW(const WCHAR *n) { if (!n) return fake_module(NULL); char t[128]; size_t l = utf16_to_utf8(n, -1, t, 127); t[l] = 0; return fake_module(t); }
static MS BOOL GetModuleHandleExW(DWORD fl, const void *n, HANDLE *out) {
    if (fl & 4) { *out = (HANDLE)g_image_base; return 1; }
    HANDLE h = n ? GetModuleHandleW(n) : (HANDLE)g_image_base; *out = h; return h != NULL;
}
static MS void *GetProcAddress(HANDLE m, const char *name) { (void)m; (void)name; seterr(127); return NULL; }
static MS HANDLE LoadLibraryA(const char *name) { HANDLE h = fake_module(name); if (!h) seterr(126); return h; }
static MS HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
static MS DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
static MS HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
static MS DWORD GetCurrentThreadId(void) { return host_tid(); }
static MS DWORD GetActiveProcessorCount(uint16_t g) { (void)g; long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (DWORD)n : 1; }
static MS BOOL GetComputerNameA(char *buf, DWORD *n) { char h[256]; gethostname(h, sizeof h); size_t l = strlen(h); if (l >= *n) { *n = (DWORD)l + 1; seterr(111); return 0; } memcpy(buf, h, l + 1); *n = (DWORD)l; return 1; }
static MS BOOL SetEnvironmentVariableA(const char *n, const char *v) { if (v) setenv(n, v, 1); else unsetenv(n); return 1; }
static MS BOOL CreateProcessW(void *a, void *b, void *c, void *d, BOOL e, DWORD f, void *g, void *h, void *i, void *j) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j; seterr(5); return 0; }
static MS void *SetUnhandledExceptionFilter(void *f) { (void)f; return NULL; }
static MS int __C_specific_handler(void *a, void *b, void *c, void *d) { (void)a;(void)b;(void)c;(void)d; return 1; /* ExceptionContinueSearch */ }
static MS uint16_t RtlCaptureStackBackTrace(DWORD skip, DWORD n, void **p, DWORD *h) { (void)skip;(void)n;(void)p; if (h) *h = 0; return 0; }
static MS void *RtlLookupFunctionEntry(uint64_t pc, uint64_t *base, void *hist) { (void)pc;(void)hist; if (base) *base = 0; return NULL; }
static MS void *RtlVirtualUnwind(DWORD t, uint64_t b, uint64_t pc, void *fe, void *ctx, void **hd, uint64_t *ef, void *cp) { (void)t;(void)b;(void)pc;(void)fe;(void)ctx;(void)hd;(void)ef;(void)cp; return NULL; }
static MS BOOL GetThreadContext(HANDLE t, void *ctx) { (void)t; (void)ctx; seterr(5); return 0; }
static MS DWORD SuspendThread(HANDLE t) { (void)t; return (DWORD)-1; }
static MS DWORD ResumeThread(HANDLE t) { (void)t; return (DWORD)-1; }
static MS BOOL SetThreadPriority(HANDLE t, int p) { (void)t; (void)p; return 1; }
static MS BOOL GetThreadTimes(HANDLE t, uint64_t *c, uint64_t *e, uint64_t *k, uint64_t *u) { (void)t; *c = *e = *k = *u = 0; return 1; }

/* ------------------------------------------------------------------ virtual memory (only the image: MinGW pseudo-reloc support) */
extern struct img_section { uintptr_t va; size_t size; uint32_t chars; char name[9]; } g_secs[32];
extern int g_nsecs; extern size_t g_image_size; extern uint8_t *g_pageprot;
static int prot_from_win(DWORD p) {
    switch (p & 0xFF) { case 0x01: return PROT_NONE; case 0x02: return PROT_READ; case 0x04: case 0x08: return PROT_READ | PROT_WRITE;
                        case 0x10: return PROT_EXEC; case 0x20: return PROT_READ | PROT_EXEC; default: return PROT_READ | PROT_WRITE | PROT_EXEC; }
}
static MS BOOL VirtualProtect(void *addr, size_t size, DWORD np, DWORD *old) {
    uintptr_t a = (uintptr_t)addr & ~0xFFFul, e = ((uintptr_t)addr + size + 0xFFF) & ~0xFFFul;
    int inimg = a >= g_image_base && e <= g_image_base + g_image_size;
    if (old) *old = inimg ? g_pageprot[(a - g_image_base) / 4096] : 0x04;
    if (mprotect((void *)a, e - a, prot_from_win(np))) { seterr(errno_to_win(errno)); return 0; }
    if (inimg) memset(g_pageprot + (a - g_image_base) / 4096, np & 0xFF, (e - a) / 4096);
    return 1;
}
struct mbi { uint64_t base, allocbase; DWORD allocprot, pad; uint64_t size; DWORD state, protect, type, pad2; };
static MS size_t VirtualQuery(const void *addr, struct mbi *m, size_t len) {
    (void)len; uintptr_t a = (uintptr_t)addr;
    memset(m, 0, sizeof *m);
    if (a >= g_image_base && a < g_image_base + g_image_size) {
        uintptr_t rva = a - g_image_base; m->allocbase = g_image_base; m->allocprot = 0x80;
        m->base = a & ~0xFFFul; m->state = 0x1000; m->type = 0x1000000;
        m->size = 0x1000; m->protect = g_pageprot[rva / 4096];
        for (int i = 0; i < g_nsecs; i++)
            if (rva >= g_secs[i].va && rva < g_secs[i].va + g_secs[i].size) {
                uintptr_t end = (g_image_base + g_secs[i].va + g_secs[i].size + 0xFFF) & ~0xFFFul; m->size = end - m->base; break;
            }
        return sizeof *m;
    }
    m->base = a & ~0xFFFul; m->allocbase = m->base; m->size = 0x1000; m->state = 0x10000; /* MEM_FREE */ m->protect = 1;
    return sizeof *m;
}

/* ------------------------------------------------------------------ sync primitives */
static pthread_mutex_t *lazy_mutex(void **slot, int recursive) {
    void *v = atomic_load_explicit((_Atomic(void *) *)slot, memory_order_acquire);
    if (v) return v;
    pthread_mutex_t *m = malloc(sizeof *m); pthread_mutexattr_t at; pthread_mutexattr_init(&at);
    if (recursive) pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &at); pthread_mutexattr_destroy(&at);
    void *expect = NULL;
    if (!atomic_compare_exchange_strong((_Atomic(void *) *)slot, &expect, m)) { pthread_mutex_destroy(m); free(m); return expect; }
    return m;
}
static pthread_cond_t *lazy_cond(void **slot) {
    void *v = atomic_load_explicit((_Atomic(void *) *)slot, memory_order_acquire);
    if (v) return v;
    pthread_cond_t *c = malloc(sizeof *c); pthread_cond_init(c, NULL);
    void *expect = NULL;
    if (!atomic_compare_exchange_strong((_Atomic(void *) *)slot, &expect, c)) { pthread_cond_destroy(c); free(c); return expect; }
    return c;
}
/* CRITICAL_SECTION: first 8 bytes hold a pointer to a recursive pthread mutex */
static MS void InitializeCriticalSection(void **cs) { *cs = NULL; lazy_mutex(cs, 1); }
static MS void DeleteCriticalSection(void **cs) { if (*cs) { pthread_mutex_destroy(*cs); free(*cs); *cs = NULL; } }
static MS void EnterCriticalSection(void **cs) { ST(ST_CSENTER); pthread_mutex_lock(lazy_mutex(cs, 1)); }
static MS void LeaveCriticalSection(void **cs) { pthread_mutex_unlock(*cs); }
/* SRWLOCK / CONDITION_VARIABLE: one pointer, zero-initialised statically, allocated on first use */
static MS void InitializeSRWLock(void **l) { *l = NULL; }
static MS void AcquireSRWLockExclusive(void **l) { ST(ST_SRWACQ); pthread_mutex_lock(lazy_mutex(l, 0)); }
static MS void ReleaseSRWLockExclusive(void **l) { pthread_mutex_unlock(*l); }
static MS uint8_t TryAcquireSRWLockExclusive(void **l) { return pthread_mutex_trylock(lazy_mutex(l, 0)) == 0; }
static MS void InitializeConditionVariable(void **c) { *c = NULL; }
static MS void WakeConditionVariable(void **c) { ST(ST_WAKE); pthread_cond_signal(lazy_cond(c)); }
static MS void WakeAllConditionVariable(void **c) { ST(ST_WAKEALL); pthread_cond_broadcast(lazy_cond(c)); }
static int cv_wait(void **cv, pthread_mutex_t *m, DWORD ms) { ST(ST_SLEEPCV);
    pthread_cond_t *c = lazy_cond(cv);
    if (ms == INFINITE) { pthread_cond_wait(c, m); return 1; }
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    if (pthread_cond_timedwait(c, m, &ts) == ETIMEDOUT) { seterr(1460); return 0; }
    return 1;
}
static MS BOOL SleepConditionVariableCS(void **cv, void **cs, DWORD ms) { return cv_wait(cv, lazy_mutex(cs, 1), ms); }
static MS BOOL SleepConditionVariableSRW(void **cv, void **srw, DWORD ms, DWORD flags) { (void)flags; return cv_wait(cv, lazy_mutex(srw, 0), ms); }

/* ------------------------------------------------------------------ TLS */
static MS DWORD TlsAlloc(void) { pthread_key_t k; if (pthread_key_create(&k, NULL)) return 0xFFFFFFFFu; return (DWORD)k; }
static MS void *TlsGetValue(DWORD i) { seterr(0); return pthread_getspecific(i); }
static MS BOOL TlsSetValue(DWORD i, void *v) { return pthread_setspecific(i, v) == 0; }

/* ------------------------------------------------------------------ threads + handles */
#define THREAD_MAGIC 0x54485244u
struct thandle { uint32_t magic; pthread_t th; DWORD tid; int done; DWORD ret; pthread_mutex_t m; pthread_cond_t c; MS DWORD (*fn)(void *); void *arg; };
static _Thread_local struct thandle *t_self;
static void *thread_main(void *p) {
    struct thandle *t = p; t_self = t;
        DWORD r = t->fn(t->arg);
    pthread_mutex_lock(&t->m); t->ret = r; t->done = 1; pthread_cond_broadcast(&t->c); pthread_mutex_unlock(&t->m);
    return NULL;
}
HANDLE host_create_thread(size_t stack, MS DWORD (*fn)(void *), void *arg, DWORD *tid) {
    struct thandle *t = calloc(1, sizeof *t); t->magic = THREAD_MAGIC; t->fn = fn; t->arg = arg;
    pthread_mutex_init(&t->m, NULL); pthread_cond_init(&t->c, NULL);
    pthread_attr_t at; pthread_attr_init(&at);
    if (stack < (2u << 20)) stack = 2u << 20;
    pthread_attr_setstacksize(&at, stack);
    if (pthread_create(&t->th, &at, thread_main, t)) { free(t); seterr(8); return NULL; }
    t->tid = 0;
    if (tid) *tid = t->tid;
    pthread_attr_destroy(&at);
    return t;
}
void host_end_thread(DWORD ret) {   /* _endthreadex */
    struct thandle *t = t_self;
    if (t) { pthread_mutex_lock(&t->m); t->ret = ret; t->done = 1; pthread_cond_broadcast(&t->c); pthread_mutex_unlock(&t->m); }
    pthread_exit(NULL);
}
static MS HANDLE CreateThread(void *sa, size_t stack, MS DWORD (*fn)(void *), void *arg, DWORD flags, DWORD *tid) { (void)sa; (void)flags; return host_create_thread(stack, fn, arg, tid); }
static int is_thread(HANDLE h) { return (uintptr_t)h > 0x10000 && ((struct thandle *)h)->magic == THREAD_MAGIC; }
static MS DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    if (!is_thread(h)) return 0;
    struct thandle *t = h; pthread_mutex_lock(&t->m);
    DWORD rc = 0;
    while (!t->done) {
        if (ms == INFINITE) pthread_cond_wait(&t->c, &t->m);
        else {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&t->c, &t->m, &ts) == ETIMEDOUT && !t->done) { rc = WAIT_TIMEOUT; break; }
        }
    }
    pthread_mutex_unlock(&t->m);
    return rc;
}
static MS BOOL DuplicateHandle(HANDLE sp, HANDLE h, HANDLE tp, HANDLE *out, DWORD acc, BOOL inh, DWORD opt) {
    (void)sp; (void)tp; (void)acc; (void)inh; (void)opt;
    if ((intptr_t)h == -2) {                       /* the current thread: hand back a handle that identifies it */
        struct thandle *t = calloc(1, sizeof *t); t->magic = THREAD_MAGIC; t->th = pthread_self(); t->tid = GetCurrentThreadId();
        pthread_mutex_init(&t->m, NULL); pthread_cond_init(&t->c, NULL); *out = t; return 1;
    }
    *out = h; return 1;
}
static MS BOOL CloseHandle(HANDLE h) {
    if ((uintptr_t)h < 0x10000) { if ((intptr_t)h >= 4) close((int)((uintptr_t)h - 4)); return 1; }
    if (is_thread(h)) { struct thandle *t = h; if (t->done || !t->fn) { pthread_mutex_destroy(&t->m); pthread_cond_destroy(&t->c); t->magic = 0; free(t); } else pthread_detach(t->th); }
    return 1;
}

/* ------------------------------------------------------------------ fibers (the game's m3_coro_* coroutines) */
/* GetCurrentFiber() is a pointer to this; GetFiberData() reads its first qword -- so param must stay first */
struct fiber { void *param; void *sp; void *stack; size_t size; MS void (*fn)(void *); };
static _Thread_local struct fiber *t_cur;
void *host_current_fiber(void) { return t_cur; }
extern void fiber_swap(void **save, void *to);
extern void fiber_boot(void);
void fiber_main(struct fiber *f) {
    f->fn(f->param);
    log_msg("a fiber function returned (that ends the thread on Windows)");
    host_end_thread(0);
}
static MS void *ConvertThreadToFiberEx(void *param, DWORD flags) {
    (void)flags; struct fiber *f = calloc(1, sizeof *f); f->param = param; t_cur = f; return f;
}
static MS BOOL IsThreadAFiber(void) { return t_cur != NULL; }
static MS void *CreateFiberEx(size_t commit, size_t reserve, DWORD flags, MS void (*fn)(void *), void *param) {
    (void)commit; (void)flags;
    size_t size = reserve > (2u << 20) ? reserve : (2u << 20); size = (size + 0xFFFF) & ~0xFFFFul;
    uint8_t *stk = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (stk == MAP_FAILED) { seterr(8); return NULL; }
    struct fiber *f = calloc(1, sizeof *f); f->stack = stk; f->size = size; f->fn = fn; f->param = param;
    uintptr_t top = ((uintptr_t)stk + size) & ~15ul;
    uint64_t *sp = (uint64_t *)(top - 72);        /* see fiber.S: 6 saved regs, return address, alignment */
    for (int i = 0; i < 6; i++) sp[i] = 0;
    sp[3] = (uint64_t)(uintptr_t)f;               /* r12 slot (pop order r15 r14 r13 r12 rbx rbp) */
    sp[6] = (uint64_t)(uintptr_t)fiber_boot;
    f->sp = sp;
    return f;
}
static MS void SwitchToFiber(void *to) { ST(ST_SWITCH);
    struct fiber *t = to, *me = t_cur;
    if (!me || !t) { log_msg("SwitchToFiber(%p) on %s", to, me ? "a NULL target" : "a thread that is not a fiber"); abort(); }
    t_cur = t;
    fiber_swap(&me->sp, t->sp);
    t_cur = me;
}
static MS void DeleteFiber(void *fp) {
    struct fiber *f = fp; if (f == t_cur) return;
    if (f->stack) munmap(f->stack, f->size);
    free(f);
}

/* ------------------------------------------------------------------ files */
static HANDLE fd_to_handle(int fd) { return (HANDLE)(uintptr_t)(fd + 4); }
static int h_to_fd(HANDLE h) { return (int)((uintptr_t)h - 4); }
static HANDLE create_file(const char *path, DWORD access, DWORD disp) {
    int fl = 0;
    int rd = access & 0x80000000u, wr = access & 0x40000000u;
    fl = rd && wr ? O_RDWR : (wr ? O_WRONLY : O_RDONLY);
    switch (disp) { case 1: fl |= O_CREAT | O_EXCL; break; case 2: fl |= O_CREAT | O_TRUNC; break; case 4: fl |= O_CREAT; break; case 5: fl |= O_TRUNC; break; default: break; }
    int fd = open(path, fl, 0666);
    if (fd < 0) { seterr(errno_to_win(errno)); return INVALID_HANDLE_VALUE; }
    return fd_to_handle(fd);
}
static MS HANDLE CreateFileA(const char *n, DWORD a, DWORD s, void *sa, DWORD d, DWORD f, HANDLE t) { (void)s;(void)sa;(void)f;(void)t; char p[2048]; win_path_to_unix(n, p, sizeof p); return create_file(p, a, d); }
static MS HANDLE CreateFileW(const WCHAR *n, DWORD a, DWORD s, void *sa, DWORD d, DWORD f, HANDLE t) { (void)s;(void)sa;(void)f;(void)t; char p[2048]; win_wpath_to_unix(n, p, sizeof p); return create_file(p, a, d); }
static MS BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov) {
    (void)ov; ssize_t r = write(h_to_fd(h), buf, n);
    if (r < 0) { seterr(errno_to_win(errno)); if (written) *written = 0; return 0; }
    if (written) *written = (DWORD)r; return 1;
}
static MS BOOL FlushFileBuffers(HANDLE h) { fsync(h_to_fd(h)); return 1; }
static MS DWORD SetFilePointer(HANDLE h, int32_t lo, int32_t *hi, DWORD method) {
    off_t off = hi ? (((int64_t)*hi << 32) | (uint32_t)lo) : lo;
    off_t r = lseek(h_to_fd(h), off, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) { seterr(errno_to_win(errno)); return 0xFFFFFFFFu; }
    if (hi) *hi = (int32_t)(r >> 32);
    return (DWORD)r;
}
static MS DWORD GetFileSize(HANDLE h, DWORD *hi) { struct stat st; if (fstat(h_to_fd(h), &st)) return 0xFFFFFFFFu; if (hi) *hi = (DWORD)((uint64_t)st.st_size >> 32); return (DWORD)st.st_size; }
static MS BOOL SetEndOfFile(HANDLE h) { off_t p = lseek(h_to_fd(h), 0, SEEK_CUR); return ftruncate(h_to_fd(h), p) == 0; }
static MS BOOL DeleteFileW(const WCHAR *n) { char p[2048]; win_wpath_to_unix(n, p, sizeof p); if (unlink(p)) { seterr(errno_to_win(errno)); return 0; } return 1; }
static MS BOOL MoveFileExW(const WCHAR *a, const WCHAR *b, DWORD fl) {
    char pa[2048], pb[2048]; win_wpath_to_unix(a, pa, sizeof pa); win_wpath_to_unix(b, pb, sizeof pb);
    if (!(fl & 1) && access(pb, F_OK) == 0) { seterr(183); return 0; }
    if (rename(pa, pb)) { seterr(errno_to_win(errno)); return 0; } return 1;
}
static MS DWORD GetFileAttributesW(const WCHAR *n) { char p[2048]; win_wpath_to_unix(n, p, sizeof p); struct stat st; if (stat(p, &st)) { seterr(errno_to_win(errno)); return 0xFFFFFFFFu; } return S_ISDIR(st.st_mode) ? 0x10 : 0x80; }
static MS BOOL SetFileAttributesW(const WCHAR *n, DWORD a) { (void)n; (void)a; return 1; }
static MS BOOL CreateDirectoryW(const WCHAR *n, void *sa) { (void)sa; char p[2048]; win_wpath_to_unix(n, p, sizeof p); if (mkdir(p, 0777)) { seterr(errno_to_win(errno)); return 0; } return 1; }

struct find { DIR *d; char pattern[512]; char dir[2048]; };
#pragma pack(push, 4)
struct find_data { DWORD attrs; uint64_t ct, at, wt; DWORD size_hi, size_lo, res0, res1; WCHAR name[260]; WCHAR alt[14]; };
#pragma pack(pop)
static int find_fill(struct find *f, struct find_data *out) {
    struct dirent *e;
    while ((e = readdir(f->d))) {
        if (fnmatch(f->pattern, e->d_name, FNM_CASEFOLD) != 0) continue;
        char full[4200]; snprintf(full, sizeof full, "%s/%s", f->dir, e->d_name); struct stat st;
        memset(out, 0, sizeof *out);
        if (stat(full, &st) == 0) { out->attrs = S_ISDIR(st.st_mode) ? 0x10 : 0x80; out->size_lo = (DWORD)st.st_size; out->size_hi = (DWORD)((uint64_t)st.st_size >> 32);
            out->wt = (uint64_t)st.st_mtime * 10000000ull + 116444736000000000ull; } else out->attrs = 0x80;
        utf8_to_utf16(e->d_name, -1, out->name, 259);
        return 1;
    }
    return 0;
}
static MS HANDLE FindFirstFileW(const WCHAR *pat, struct find_data *out) {
    char p[2048]; win_wpath_to_unix(pat, p, sizeof p);
    struct find *f = calloc(1, sizeof *f);
    char *slash = strrchr(p, '/');
    if (slash) { *slash = 0; snprintf(f->dir, sizeof f->dir, "%s", p[0] ? p : "/"); snprintf(f->pattern, sizeof f->pattern, "%s", slash + 1); }
    else { snprintf(f->dir, sizeof f->dir, "."); snprintf(f->pattern, sizeof f->pattern, "%s", p); }
    if (!strcmp(f->pattern, "*.*")) strcpy(f->pattern, "*");
    f->d = opendir(f->dir);
    if (!f->d) { seterr(3); free(f); return INVALID_HANDLE_VALUE; }
    if (!find_fill(f, out)) { closedir(f->d); free(f); seterr(2); return INVALID_HANDLE_VALUE; }
    return f;
}
static MS BOOL FindNextFileW(HANDLE h, struct find_data *out) { if (!find_fill(h, out)) { seterr(18); return 0; } return 1; }
static MS BOOL FindClose(HANDLE h) { struct find *f = h; if (f && f != INVALID_HANDLE_VALUE) { closedir(f->d); free(f); } return 1; }

const struct import_entry k32_table[] = {
#define E(n) { #n, (void *)n }
    E(AcquireSRWLockExclusive), E(CloseHandle), E(ConvertThreadToFiberEx), E(CreateDirectoryW), E(CreateFiberEx), E(CreateFileA), E(CreateFileW),
    E(CreateProcessW), E(CreateThread), E(DeleteCriticalSection), E(DeleteFiber), E(DeleteFileW), E(DuplicateHandle), E(EnterCriticalSection),
    E(FindClose), E(FindFirstFileW), E(FindNextFileW), E(FlushFileBuffers), E(GetActiveProcessorCount), E(GetCommandLineW), E(GetComputerNameA),
    E(GetCurrentProcess), E(GetCurrentProcessId), E(GetCurrentThread), E(GetCurrentThreadId), E(GetFileAttributesW), E(GetFileSize), E(GetLastError),
    E(GetModuleFileNameA), E(GetModuleFileNameW), E(GetModuleHandleA), E(GetModuleHandleExW), E(GetModuleHandleW), E(GetProcAddress),
    E(GetSystemTimeAsFileTime), E(GetThreadContext), E(GetThreadTimes), E(GetTickCount64), E(InitializeConditionVariable), E(InitializeCriticalSection),
    E(InitializeSRWLock), E(IsDBCSLeadByteEx), E(IsThreadAFiber), E(LeaveCriticalSection), E(LoadLibraryA), E(MoveFileExW), E(MultiByteToWideChar),
    E(QueryPerformanceCounter), E(QueryPerformanceFrequency), E(ReleaseSRWLockExclusive), E(ResumeThread), E(RtlCaptureStackBackTrace),
    E(RtlLookupFunctionEntry), E(RtlVirtualUnwind), E(SetEndOfFile), E(SetEnvironmentVariableA), E(SetFileAttributesW), E(SetFilePointer),
    E(SetLastError), E(SetThreadPriority), E(SetUnhandledExceptionFilter), E(Sleep), E(SleepConditionVariableCS), E(SleepConditionVariableSRW),
    E(SuspendThread), E(SwitchToFiber), E(TlsAlloc), E(TlsGetValue), E(TlsSetValue), E(TryAcquireSRWLockExclusive), E(VirtualProtect),
    E(VirtualQuery), E(WaitForSingleObject), E(WakeAllConditionVariable), E(WakeConditionVariable), E(WideCharToMultiByte), E(WriteFile),
    E(__C_specific_handler),
    { 0, 0 }
};
