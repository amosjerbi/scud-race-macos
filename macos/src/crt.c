/* msvcrt on macOS: only what scudrace.exe imports. Win64 `long` is 32 bits, FILE is 48 bytes, paths use backslashes. */
#include "common.h"
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <locale.h>
#include <signal.h>
#include <wchar.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>

extern HANDLE host_create_thread(size_t stack, MS DWORD (*fn)(void *), void *arg, DWORD *tid);
extern void host_end_thread(DWORD ret);
extern void ms_setjmp(void), ms_longjmp(void);

/* ------------------------------------------------------------------ stdio */
static struct { char b[48]; } fake_iob[3];
static FILE *F(void *f) {
    if ((char *)f >= (char *)fake_iob && (char *)f < (char *)fake_iob + sizeof fake_iob) {
        switch (((char *)f - (char *)fake_iob) / 48) { case 0: return stdin; case 1: return stdout; default: return stderr; }
    }
    return f;
}
static MS void *__iob_func(void) { return fake_iob; }

static FILE *open_unix(const char *wpath, const char *mode) {
    char p[2048]; win_path_to_unix(wpath, p, sizeof p); return fopen(p, mode);
}
static void wmode(const WCHAR *w, char *out, size_t cap) { size_t n = utf16_to_utf8(w, -1, out, cap - 1); out[n] = 0; }
static MS FILE *fopen_ms(const char *p, const char *m) { return open_unix(p, m); }
static MS FILE *_wfopen(const WCHAR *p, const WCHAR *m) { char pp[2048], mm[16]; win_wpath_to_unix(p, pp, sizeof pp); wmode(m, mm, sizeof mm); return fopen(pp, mm); }
static MS int _wfopen_s(FILE **out, const WCHAR *p, const WCHAR *m) { *out = _wfopen(p, m); return *out ? 0 : errno; }
/* The game points stderr/stdout at NUL because a Windows GUI app has no console; on macOS keep the diagnostics. */
static int silencing_std(void *f, const char *unixpath) { FILE *x = F(f); return (x == stderr || x == stdout) && !strcmp(unixpath, "/dev/null") && !getenv("SCUD_HOST_ALLOW_SILENCE"); }
static MS FILE *freopen_ms(const char *p, const char *m, void *f) { char pp[2048]; win_path_to_unix(p, pp, sizeof pp); if (silencing_std(f, pp)) return F(f); return freopen(pp, m, F(f)); }
static MS FILE *_wfreopen(const WCHAR *p, const WCHAR *m, void *f) { char pp[2048], mm[16]; win_wpath_to_unix(p, pp, sizeof pp); wmode(m, mm, sizeof mm); if (silencing_std(f, pp)) return F(f); return freopen(pp, mm, F(f)); }
static MS int _wfreopen_s(FILE **out, const WCHAR *p, const WCHAR *m, void *f) { *out = _wfreopen(p, m, f); return *out ? 0 : errno; }
static MS int fclose_ms(void *f) { FILE *x = F(f); if (x == stdin || x == stdout || x == stderr) return 0; return fclose(x); }
static MS int feof_ms(void *f) { return feof(F(f)); }
static MS int ferror_ms(void *f) { return ferror(F(f)); }
static MS int fflush_ms(void *f) { return fflush(f ? F(f) : NULL); }
static MS int fgetc_ms(void *f) { return fgetc(F(f)); }
static MS int getc_ms(void *f) { return getc(F(f)); }
static MS int ungetc_ms(int c, void *f) { return ungetc(c, F(f)); }
static MS char *fgets_ms(char *b, int n, void *f) { return fgets(b, n, F(f)); }
static MS int fputc_ms(int c, void *f) { return fputc(c, F(f)); }
static MS int fputs_ms(const char *s, void *f) { return fputs(s, F(f)); }
static MS size_t fread_ms(void *b, size_t s, size_t n, void *f) { return fread(b, s, n, F(f)); }
static MS size_t fwrite_ms(const void *b, size_t s, size_t n, void *f) { return fwrite(b, s, n, F(f)); }
static MS int fseek_ms(void *f, int32_t off, int whence) { return fseek(F(f), off, whence); }
static MS int32_t ftell_ms(void *f) { return (int32_t)ftell(F(f)); }
static MS int fgetpos_ms(void *f, fpos_t *p) { return fgetpos(F(f), p); }
static MS int fsetpos_ms(void *f, const fpos_t *p) { return fsetpos(F(f), p); }
static MS void rewind_ms(void *f) { rewind(F(f)); }
static MS int setvbuf_ms(void *f, char *buf, int mode, size_t size) {
    int m = mode == 0 ? _IOFBF : (mode == 0x40 ? _IOLBF : _IONBF); return setvbuf(F(f), buf, m, size);
}
static MS FILE *tmpfile_ms(void) { return tmpfile(); }
static MS int _fileno_ms(void *f) { return fileno(F(f)); }
static MS int64_t _filelengthi64(int fd) { struct stat st; return fstat(fd, &st) ? -1 : (int64_t)st.st_size; }
static MS int remove_ms(const char *p) { char pp[2048]; win_path_to_unix(p, pp, sizeof pp); return remove(pp); }
static MS int rename_ms(const char *a, const char *b) { char pa[2048], pb[2048]; win_path_to_unix(a, pa, sizeof pa); win_path_to_unix(b, pb, sizeof pb); return rename(pa, pb); }
static MS int _mkdir(const char *p) { char pp[2048]; win_path_to_unix(p, pp, sizeof pp); return mkdir(pp, 0777); }
struct ms_stat64 { uint32_t st_dev; uint16_t st_ino, st_mode; int16_t st_nlink, st_uid, st_gid; uint16_t pad; uint32_t st_rdev; uint32_t pad2; int64_t st_size, at_, mt_, ct_; };
static MS int _wstat64(const WCHAR *p, struct ms_stat64 *o) {
    char pp[2048]; win_wpath_to_unix(p, pp, sizeof pp); struct stat st; if (stat(pp, &st)) return -1;
    memset(o, 0, sizeof *o); o->st_dev = st.st_dev; o->st_mode = (st.st_mode & S_IFDIR) ? 0x41ED : 0x81A4; o->st_nlink = 1;
    o->st_size = st.st_size; o->at_ = st.st_atime; o->mt_ = st.st_mtime; o->ct_ = st.st_ctime; return 0;
}
struct ms_utimbuf64 { int64_t actime, modtime; };
static MS int _utime64(const char *p, const struct ms_utimbuf64 *t) {
    char pp[2048]; win_path_to_unix(p, pp, sizeof pp); struct timeval tv[2];
    if (t) { tv[0].tv_sec = t->actime; tv[1].tv_sec = t->modtime; tv[0].tv_usec = tv[1].tv_usec = 0; return utimes(pp, tv); }
    return utimes(pp, NULL);
}

/* ------------------------------------------------------------------ printf (msvcrt semantics) */
struct sbuf { char *p; size_t n, cap; };
static void sb_put(struct sbuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void ms_format(struct sbuf *out, const char *f, __builtin_ms_va_list ap) {
    while (*f) {
        if (*f != '%') { const char *s = f; while (*f && *f != '%') f++; sb_put(out, s, f - s); continue; }
        const char *start = f++;
        char flags[8] = ""; int nf = 0, width = -1, prec = -1;
        while (*f && strchr("-+ #0", *f) && nf < 7) flags[nf++] = *f++;
        flags[nf] = 0;
        if (*f == '*') { width = __builtin_va_arg(ap, int); f++; if (width < 0) { width = -width; if (!strchr(flags, '-')) strcat(flags, "-"); } }
        else if (*f >= '0' && *f <= '9') { width = 0; while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0'); }
        if (*f == '.') { f++; prec = 0; if (*f == '*') { prec = __builtin_va_arg(ap, int); f++; } else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); }
        int len = 0; /* 1=h 2=hh 3=l(32 on win64) 4=ll/I64/z/j/t(64) 5=I32 6=w */
        for (;;) {
            if (*f == 'h') { len = f[1] == 'h' ? 2 : 1; f += len == 2 ? 2 : 1; }
            else if (*f == 'l') { if (f[1] == 'l') { len = 4; f += 2; } else { len = 3; f++; } }
            else if (*f == 'I' && f[1] == '6' && f[2] == '4') { len = 4; f += 3; }
            else if (*f == 'I' && f[1] == '3' && f[2] == '2') { len = 5; f += 3; }
            else if (*f == 'I') { len = 4; f++; }
            else if (*f == 'z' || *f == 'j' || *f == 't') { len = 4; f++; }
            else if (*f == 'w') { len = 6; f++; }
            else if (*f == 'L') { f++; }
            else break;
        }
        char conv = *f ? *f++ : 0;
        char fmt[64], tmp[512]; char *big = NULL;
        #define BUILD(c) do { char *q = fmt; *q++ = '%'; for (char *z = flags; *z; z++) *q++ = *z; \
            if (width >= 0) q += sprintf(q, "%d", width); if (prec >= 0) q += sprintf(q, ".%d", prec); strcpy(q, c); } while (0)
        switch (conv) {
        case 'd': case 'i': { long long v = len == 4 ? __builtin_va_arg(ap, long long) : (long long)__builtin_va_arg(ap, int);
            if (len == 1) v = (short)v; else if (len == 2) v = (signed char)v; BUILD("lld"); int n = snprintf(tmp, sizeof tmp, fmt, v); sb_put(out, tmp, n < (int)sizeof tmp ? n : (int)sizeof tmp - 1); break; }
        case 'u': case 'o': case 'x': case 'X': { unsigned long long v = len == 4 ? __builtin_va_arg(ap, unsigned long long) : (unsigned long long)__builtin_va_arg(ap, unsigned);
            if (len == 1) v = (unsigned short)v; else if (len == 2) v = (unsigned char)v; char c[4] = { 'l', 'l', conv, 0 }; BUILD(c);
            int n = snprintf(tmp, sizeof tmp, fmt, v); sb_put(out, tmp, n < (int)sizeof tmp ? n : (int)sizeof tmp - 1); break; }
        case 'c': case 'C': { int v = __builtin_va_arg(ap, int); if (conv == 'C' || len == 3 || len == 6) { WCHAR w = (WCHAR)v; char u[8]; size_t n = utf16_to_utf8(&w, 1, u, 7); u[n] = 0; BUILD("s"); int r = snprintf(tmp, sizeof tmp, fmt, u); sb_put(out, tmp, r); }
            else { BUILD("c"); int r = snprintf(tmp, sizeof tmp, fmt, v); sb_put(out, tmp, r > 0 ? r : 1); } break; }
        case 's': case 'S': { void *a = __builtin_va_arg(ap, void *); const char *s;
            int wide = (conv == 'S') || len == 3 || len == 6;
            if (!a) s = "(null)"; else if (wide) { size_t n = utf16_to_utf8(a, -1, NULL, 0); big = malloc(n + 1); n = utf16_to_utf8(a, -1, big, n); big[n] = 0; s = big; } else s = a;
            BUILD("s"); int n = snprintf(NULL, 0, fmt, s); char *buf = malloc(n + 1); snprintf(buf, n + 1, fmt, s); sb_put(out, buf, n); free(buf); free(big); break; }
        case 'p': { void *v = __builtin_va_arg(ap, void *); int n = snprintf(tmp, sizeof tmp, "%016llX", (unsigned long long)(uintptr_t)v); sb_put(out, tmp, n); break; }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': { double v = __builtin_va_arg(ap, double); char c[2] = { conv, 0 }; BUILD(c);
            int n = snprintf(NULL, 0, fmt, v); char *buf = malloc(n + 1); snprintf(buf, n + 1, fmt, v); sb_put(out, buf, n); free(buf); break; }
        case '%': sb_put(out, "%", 1); break;
        case 'n': (void)__builtin_va_arg(ap, void *); break;
        default: sb_put(out, start, f - start); break;
        }
    }
}
static MS int vfprintf_ms(void *f, const char *fmt, __builtin_ms_va_list ap) {
    struct sbuf b = { 0 }; ms_format(&b, fmt, ap);
    size_t n = b.n; if (b.p) fwrite(b.p, 1, n, F(f)); free(b.p); return (int)n;
}
static MS int fprintf_ms(void *f, const char *fmt, ...) {
    __builtin_ms_va_list ap; __builtin_ms_va_start(ap, fmt);
    int r = vfprintf_ms(f, fmt, ap); __builtin_ms_va_end(ap); return r;
}

/* ------------------------------------------------------------------ strings / numbers */
static MS int isalnum_ms(int c) { return (unsigned)c < 256 && isalnum(c); }
static MS int isspace_ms(int c) { return (unsigned)c < 256 && isspace(c); }
static MS int isxdigit_ms(int c) { return (unsigned)c < 256 && isxdigit(c); }
static MS int tolower_ms(int c) { return (unsigned)c < 256 ? tolower(c) : c; }
static MS size_t strlen_ms(const char *s) { return strlen(s); }
static MS int strcmp_ms(const char *a, const char *b) { return strcmp(a, b); }
static MS int strncmp_ms(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static MS char *strncpy_ms(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
static MS char *strncat_ms(char *d, const char *s, size_t n) { return strncat(d, s, n); }
static MS char *strchr_ms(const char *s, int c) { return strchr(s, c); }
static MS char *strrchr_ms(const char *s, int c) { return strrchr(s, c); }
static MS char *strstr_ms(const char *a, const char *b) { return strstr(a, b); }
static MS char *strpbrk_ms(const char *a, const char *b) { return strpbrk(a, b); }
static MS size_t strcspn_ms(const char *a, const char *b) { return strcspn(a, b); }
static MS size_t strspn_ms(const char *a, const char *b) { return strspn(a, b); }
static MS char *strerror_ms(int e) { return strerror(e); }
static MS char *_strdup_ms(const char *s) { return strdup(s); }
static MS int memcmp_ms(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static MS void *memcpy_ms(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static MS void *memmove_ms(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static MS void *memset_ms(void *d, int c, size_t n) { return memset(d, c, n); }
static MS void *malloc_ms(size_t n) { return malloc(n); }
static MS void *calloc_ms(size_t a, size_t b) { return calloc(a, b); }
static MS void *realloc_ms(void *p, size_t n) { return realloc(p, n); }
static MS void free_ms(void *p) { free(p); }
static MS double atof_ms(const char *s) { return atof(s); }
static MS int atoi_ms(const char *s) { return atoi(s); }
static MS int32_t atol_ms(const char *s) { long v = atol(s); return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v; }
static MS int32_t strtol_ms(const char *s, char **e, int b) { long v = strtol(s, e, b); return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v; }
static MS uint32_t strtoul_ms(const char *s, char **e, int b) { unsigned long v = strtoul(s, e, b); return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v; }
static MS int64_t _strtoi64_ms(const char *s, char **e, int b) { return strtoll(s, e, b); }
static MS uint64_t _strtoui64_ms(const char *s, char **e, int b) { return strtoull(s, e, b); }
static MS int64_t _wtoi64(const WCHAR *w) { char t[64]; size_t n = utf16_to_utf8(w, -1, t, 63); t[n] = 0; return atoll(t); }
static MS double _hypot_ms(double a, double b) { return hypot(a, b); }
static MS double acos_ms(double x) { return acos(x); }
static MS double atan_ms(double x) { return atan(x); }
static MS double tan_ms(double x) { return tan(x); }
static MS size_t wcslen_ms(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; }
static MS WCHAR *wcscpy_ms(WCHAR *d, const WCHAR *s) { WCHAR *r = d; while ((*d++ = *s++)) {} return r; }
static MS WCHAR *wcscat_ms(WCHAR *d, const WCHAR *s) { WCHAR *r = d; while (*d) d++; while ((*d++ = *s++)) {} return r; }
static MS WCHAR *_wcsdup_ms(const WCHAR *s) { size_t n = wcslen_ms(s) + 1; WCHAR *r = malloc(n * 2); memcpy(r, s, n * 2); return r; }
static MS int _wcsicmp_ms(const WCHAR *a, const WCHAR *b) {
    for (;; a++, b++) { int x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return x - y; if (!x) return 0; }
}
static MS void *localeconv_ms(void) { return localeconv(); }
static MS int *_errno_ms(void) { return __error(); }
static MS unsigned ___lc_codepage_func(void) { return 65001; }
static MS int ___mb_cur_max_func(void) { return 1; }

/* ------------------------------------------------------------------ qsort with a Win64 comparator */
static int qs_cmp(void *ctx, const void *a, const void *b) { return ((MS int (*)(const void *, const void *))ctx)(a, b); }
static MS void qsort_ms(void *base, size_t n, size_t sz, void *cmp) { qsort_r(base, n, sz, cmp, qs_cmp); }

/* ------------------------------------------------------------------ time */
static MS int64_t _time64(int64_t *t) { int64_t v = time(NULL); if (t) *t = v; return v; }
static MS void *_localtime64(const int64_t *t) { static _Thread_local struct tm tmv; time_t tt = (time_t)*t; return localtime_r(&tt, &tmv); }
static MS int64_t _mktime64(void *tmv) { struct tm t; memcpy(&t, tmv, 9 * sizeof(int)); t.tm_gmtoff = 0; t.tm_zone = NULL; return (int64_t)mktime(&t); }
static MS int32_t clock_ms(void) { static uint64_t t0; uint64_t n = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000000ull; if (!t0) t0 = n; return (int32_t)(n - t0); }

/* ------------------------------------------------------------------ process, startup, threads */
static int g_argc; static char **g_argv; static char *g_arg_store[64];
static char **g_environ_var; static char **g_initenv_var; static int g_fmode, g_commode;
extern char **environ;
static MS int __getmainargs(int *argc, char ***argv, char ***env, int wild, void *si) {
    (void)wild; (void)si;
    extern char g_exe_path_win[]; extern void host_get_args(int *, char ***);
    host_get_args(&g_argc, &g_argv);
    g_arg_store[0] = g_exe_path_win;
    for (int i = 1; i < g_argc && i < 63; i++) g_arg_store[i] = g_argv[i];
    g_arg_store[g_argc < 63 ? g_argc : 63] = NULL;
    *argc = g_argc < 63 ? g_argc : 63; *argv = g_arg_store; *env = environ;
    g_environ_var = g_initenv_var = environ; return 0;
}
static MS int __set_app_type(int t) { (void)t; return 0; }
static MS void *__setusermatherr(void *f) { (void)f; return NULL; }
typedef MS void (*initfn_t)(void);
static MS void _initterm(initfn_t *s, initfn_t *e) {
    for (; s < e; s++) if (*s) { if (g_trace) log_msg("_initterm: calling %p", (void *)*s); (*s)(); if (g_trace) log_msg("_initterm: %p returned", (void *)*s); }
}
static MS void _cexit(void) {}
#define MAX_ONEXIT 128
static MS void (*onexit_list[MAX_ONEXIT])(void); static int onexit_n;
static MS void *_onexit(void *fn) { if (onexit_n < MAX_ONEXIT) onexit_list[onexit_n++] = fn; return fn; }
static void run_onexit(void) { while (onexit_n > 0) { void (*MS f)(void) = onexit_list[--onexit_n]; f(); } }
static MS void exit_ms(int c) { log_msg("exit(%d) called from PE address %p", c, __builtin_return_address(0)); run_onexit(); fflush(NULL); exit(c); }
static MS void _exit_ms(int c) { log_msg("_exit(%d) called from PE address %p", c, __builtin_return_address(0)); fflush(NULL); _exit(c); }
static MS void abort_ms(void) { log_msg("abort() called from PE address %p", __builtin_return_address(0)); fflush(NULL); fputs("\n[host] the game called abort()\n", stderr); abort(); }
static MS void _amsg_exit(int c) { fprintf(stderr, "\n[host] runtime error R%d\n", c); fflush(NULL); _exit(255); }
static MS void *signal_ms(int sig, void *h) { (void)sig; (void)h; return NULL; }
static MS char *getenv_ms(const char *n) { return getenv(n); }
static MS int _putenv(const char *s) { char *d = strdup(s), *eq = strchr(d, '='); if (!eq) { free(d); return -1; } *eq = 0; if (eq[1]) setenv(d, eq + 1, 1); else unsetenv(d); free(d); return 0; }
static pthread_mutex_t lock_tab[64]; static pthread_once_t lock_once = PTHREAD_ONCE_INIT;
static void lock_init(void) { pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); for (int i = 0; i < 64; i++) pthread_mutex_init(&lock_tab[i], &a); }
static MS void _lock(int n) { pthread_once(&lock_once, lock_init); pthread_mutex_lock(&lock_tab[n & 63]); }
static MS void _unlock(int n) { pthread_mutex_unlock(&lock_tab[n & 63]); }
static MS uintptr_t _beginthreadex(void *sec, unsigned stack, MS unsigned (*fn)(void *), void *arg, unsigned flag, unsigned *tid) {
    (void)sec; (void)flag; DWORD t; HANDLE h = host_create_thread(stack, (MS DWORD (*)(void *))fn, arg, &t); if (tid) *tid = t; return (uintptr_t)h;
}
static MS void _endthreadex(unsigned r) { host_end_thread(r); }

#define E2(n, f) { n, (void *)f }
const struct import_entry crt_table[] = {
    E2("___lc_codepage_func", ___lc_codepage_func), E2("___mb_cur_max_func", ___mb_cur_max_func), E2("__getmainargs", __getmainargs),
    E2("__initenv", &g_initenv_var), E2("__iob_func", __iob_func), E2("__set_app_type", __set_app_type), E2("__setusermatherr", __setusermatherr),
    E2("_amsg_exit", _amsg_exit), E2("_beginthreadex", _beginthreadex), E2("_cexit", _cexit), E2("_commode", &g_commode), E2("_endthreadex", _endthreadex),
    E2("_environ", &g_environ_var), E2("_errno", _errno_ms), E2("_exit", _exit_ms), E2("_filelengthi64", _filelengthi64), E2("_fileno", _fileno_ms),
    E2("_fmode", &g_fmode), E2("_hypot", _hypot_ms), E2("_initterm", _initterm), E2("_localtime64", _localtime64), E2("_lock", _lock), E2("_mkdir", _mkdir),
    E2("_mktime64", _mktime64), E2("_onexit", _onexit), E2("_putenv", _putenv), E2("_setjmp", ms_setjmp), E2("_time64", _time64), E2("_unlock", _unlock),
    E2("_utime64", _utime64), E2("_wcsdup", _wcsdup_ms), E2("_wcsicmp", _wcsicmp_ms), E2("_wfopen", _wfopen), E2("_wfopen_s", _wfopen_s),
    E2("_wfreopen", _wfreopen), E2("_wfreopen_s", _wfreopen_s), E2("_wstat64", _wstat64), E2("_wtoi64", _wtoi64), E2("abort", abort_ms),
    E2("acos", acos_ms), E2("atan", atan_ms), E2("atof", atof_ms), E2("atoi", atoi_ms), E2("atol", atol_ms), E2("calloc", calloc_ms), E2("clock", clock_ms),
    E2("exit", exit_ms), E2("fclose", fclose_ms), E2("feof", feof_ms), E2("ferror", ferror_ms), E2("fflush", fflush_ms), E2("fgetc", fgetc_ms),
    E2("fgetpos", fgetpos_ms), E2("fgets", fgets_ms), E2("fopen", fopen_ms), E2("fprintf", fprintf_ms), E2("fputc", fputc_ms), E2("fputs", fputs_ms),
    E2("fread", fread_ms), E2("free", free_ms), E2("freopen", freopen_ms), E2("fseek", fseek_ms), E2("fsetpos", fsetpos_ms), E2("ftell", ftell_ms),
    E2("fwrite", fwrite_ms), E2("getc", getc_ms), E2("getenv", getenv_ms), E2("isalnum", isalnum_ms), E2("isspace", isspace_ms), E2("isxdigit", isxdigit_ms),
    E2("localeconv", localeconv_ms), E2("longjmp", ms_longjmp), E2("malloc", malloc_ms), E2("memcmp", memcmp_ms), E2("memcpy", memcpy_ms),
    E2("memmove", memmove_ms), E2("memset", memset_ms), E2("qsort", qsort_ms), E2("realloc", realloc_ms), E2("remove", remove_ms), E2("rename", rename_ms),
    E2("rewind", rewind_ms), E2("setvbuf", setvbuf_ms), E2("signal", signal_ms), E2("strchr", strchr_ms), E2("strcmp", strcmp_ms), E2("strcspn", strcspn_ms),
    E2("strerror", strerror_ms), E2("strlen", strlen_ms), E2("strncat", strncat_ms), E2("strncmp", strncmp_ms), E2("strncpy", strncpy_ms),
    E2("strpbrk", strpbrk_ms), E2("strrchr", strrchr_ms), E2("strspn", strspn_ms), E2("strstr", strstr_ms), E2("strtol", strtol_ms), E2("strtoul", strtoul_ms),
    E2("tan", tan_ms), E2("tmpfile", tmpfile_ms), E2("tolower", tolower_ms), E2("ungetc", ungetc_ms), E2("vfprintf", vfprintf_ms), E2("wcscat", wcscat_ms),
    E2("wcscpy", wcscpy_ms), E2("wcslen", wcslen_ms), E2("_strtoui64", _strtoui64_ms), E2("_strtoi64", _strtoi64_ms), E2("_strdup", _strdup_ms),
    { 0, 0 }
};
