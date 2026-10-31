/* Process entry for the arm64 build: lays out the game's data image (linked into this binary), points every import slot
 * at a thunk token, and runs the translated entry point. No .exe is read at run time. */
#include "common.h"
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <execinfo.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

uintptr_t g_image_base;
int g_trace;
char g_game_dir[1024];
char g_exe_path_win[1100];
struct img_section { uintptr_t va; size_t size; uint32_t chars; char name[9]; };
struct img_section g_secs[32];
int g_nsecs;
size_t g_image_size;
uint8_t *g_pageprot;

extern const uint8_t scud_image_blob[], scud_image_blob_end[];
extern const struct import_entry rt_table[];
uint64_t rt_import_token(const char *name, void *fn, const char *sig);

void log_msg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("[host] ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

static const struct import_entry *lookup_import(const char *name) {
    static const struct import_entry *tabs[] = { rt_table, k32_table, crt_table, misc_table, sdl_special_table, sdl_generated_table };
    for (size_t t = 0; t < sizeof tabs / sizeof *tabs; t++)
        for (const struct import_entry *e = tabs[t]; e->name; e++)
            if (!strcmp(e->name, name)) return e;
    return NULL;
}
void *host_lookup(const char *name) { const struct import_entry *e = lookup_import(name); return e ? e->fn : NULL; }

#pragma pack(push, 1)
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
#pragma pack(pop)

static uint64_t load_image(void) {
    const uint8_t *b = scud_image_blob;
    if (memcmp(b, "SCUDIMG1", 8)) { log_msg("the embedded game image is missing"); exit(1); }
    uint64_t base; uint32_t size, n; memcpy(&base, b + 8, 8); memcpy(&size, b + 16, 4); memcpy(&n, b + 20, 4);
    size_t msize = (size + 0xFFFF) & ~(size_t)0xFFFF;
    uint8_t *img = mmap((void *)(uintptr_t)base, msize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);   /* over our own reservation */
    if (img != (uint8_t *)(uintptr_t)base) { log_msg("cannot map the game image at %#llx: %s", (unsigned long long)base, strerror(errno)); exit(1); }
    const uint8_t *p = b + 24;
    for (uint32_t i = 0; i < n; i++) { uint32_t va, len; memcpy(&va, p, 4); memcpy(&len, p + 4, 4); memcpy(img + va, p + 8, len); p += 8 + len; }
    g_image_base = base; g_image_size = msize;

    int32_t lfanew; memcpy(&lfanew, img + 0x3c, 4);
    struct file_hdr *fh = (void *)(img + lfanew + 4);
    struct opt_hdr64 *oh = (void *)(fh + 1);
    struct sec_hdr *sh = (void *)((uint8_t *)oh + fh->optsz);
    g_pageprot = calloc(msize / 4096 + 1, 1); memset(g_pageprot, 0x02, msize / 4096);
    for (int i = 0; i < fh->nsec && i < 32; i++) {
        uint32_t vs = sh[i].vsize ? sh[i].vsize : sh[i].rawsize;
        g_secs[i].va = sh[i].va; g_secs[i].size = vs; g_secs[i].chars = sh[i].chars;
        memcpy(g_secs[i].name, sh[i].name, 8); g_secs[i].name[8] = 0; g_nsecs = i + 1;
        uint32_t ch = sh[i].chars; int x = !!(ch & 0x20000000), w = !!(ch & 0x80000000);
        memset(g_pageprot + sh[i].va / 4096, x ? (w ? 0x40 : 0x20) : (w ? 0x04 : 0x02), ((vs + 0xFFF) & ~0xFFFu) / 4096);
    }

    int missing = 0, total = 0;
    for (struct imp_desc *d = (void *)(img + oh->dir[1].rva); oh->dir[1].size && d->name; d++) {
        const char *dll = (const char *)img + d->name;
        uint64_t *ilt = (void *)(img + (d->oft ? d->oft : d->ft)), *iat = (void *)(img + d->ft);
        for (; *ilt; ilt++, iat++) {
            total++;
            if (*ilt >> 63) { *iat = rt_import_token("<ordinal import>", NULL, NULL); missing++; continue; }
            const char *name = (const char *)img + (*ilt & 0x7FFFFFFF) + 2;
            const struct import_entry *e = lookup_import(name);
            if (e && !e->sig) { *iat = (uint64_t)(uintptr_t)e->fn; continue; }     /* data import */
            if (!e) { missing++; if (g_trace) log_msg("unimplemented import: %s!%s", dll, name); }
            *iat = rt_import_token(name, e ? e->fn : NULL, e ? e->sig : NULL);
        }
    }
    log_msg("game image at %p (%zu KB, embedded); %d imports, %d not implemented", img, msize >> 10, total, missing);
    return base + oh->entry;
}

static void find_dirs(void) {
    const char *env = getenv("SCUDRACE_HOME");
    if (env && *env) { snprintf(g_game_dir, sizeof g_game_dir, "%s", env); return; }
    char exe[PATH_MAX] = "", real[PATH_MAX];
#ifdef __APPLE__
    uint32_t n = sizeof exe; if (_NSGetExecutablePath(exe, &n)) exe[0] = 0;
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1); exe[n > 0 ? n : 0] = 0;
#endif
    if (!exe[0] || !realpath(exe, real)) { if (!getcwd(g_game_dir, sizeof g_game_dir)) g_game_dir[0] = 0; return; }
    char *dir = dirname(real), *app = strstr(dir, ".app/Contents/MacOS");
    if (app) { char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%.*s", (int)(app - dir), dir); snprintf(g_game_dir, sizeof g_game_dir, "%s", dirname(tmp)); }
    else snprintf(g_game_dir, sizeof g_game_dir, "%s", dir);
    char t[PATH_MAX + 32]; snprintf(t, sizeof t, "%s/game/ui", g_game_dir);   /* <folder>/game/{ui,Music,...} + <folder>/roms/ */
    if (!access(t, R_OK)) { size_t l = strlen(g_game_dir); snprintf(g_game_dir + l, sizeof g_game_dir - l, "/game"); }
}

#ifdef __APPLE__
/* SCUD_HOST_SAMPLE=<file>: a thread samples the pc of every running thread each ms; written at SIGTERM ("pc" per line) */
#include <mach/mach.h>
#define PROF_MAX (1 << 21)
static uint64_t prof_buf[PROF_MAX]; static volatile uint32_t prof_n; static const char *prof_file;
static void *sampler_main(void *arg) {
    (void)arg; mach_port_t self = mach_thread_self();
    for (;;) {
        usleep(1000);
        thread_act_array_t th; mach_msg_type_number_t cnt;
        if (task_threads(mach_task_self(), &th, &cnt)) continue;
        for (mach_msg_type_number_t k = 0; k < cnt; k++) {
            if (th[k] == self) continue;
            thread_basic_info_data_t bi; mach_msg_type_number_t bn = THREAD_BASIC_INFO_COUNT;
            if (thread_info(th[k], THREAD_BASIC_INFO, (thread_info_t)&bi, &bn) || bi.run_state != TH_STATE_RUNNING) continue;
            if (thread_suspend(th[k])) continue;
            arm_thread_state64_t st; mach_msg_type_number_t sn = ARM_THREAD_STATE64_COUNT;
            if (!thread_get_state(th[k], ARM_THREAD_STATE64, (thread_state_t)&st, &sn)) {
                uint32_t i = __atomic_fetch_add(&prof_n, 1, __ATOMIC_RELAXED); if (i < PROF_MAX) prof_buf[i] = arm_thread_state64_get_pc(st);
            }
            thread_resume(th[k]);
        }
        for (mach_msg_type_number_t k = 0; k < cnt; k++) mach_port_deallocate(mach_task_self(), th[k]);
        vm_deallocate(mach_task_self(), (vm_address_t)th, cnt * sizeof *th);
    }
    return NULL;
}
static void prof_dump(int sig) {
    (void)sig; FILE *f = fopen(prof_file, "w"); uint32_t n = prof_n < PROF_MAX ? prof_n : PROF_MAX;
    extern const struct mach_header_64 _mh_execute_header;
    if (f) { fprintf(f, "base %llx\n", (unsigned long long)(uintptr_t)&_mh_execute_header); for (uint32_t i = 0; i < n; i++) fprintf(f, "%llx\n", (unsigned long long)prof_buf[i]); fclose(f); }
    _exit(0);
}
static void prof_start(void) {
    prof_file = getenv("SCUD_HOST_SAMPLE"); if (!prof_file) return;
    signal(SIGTERM, prof_dump); pthread_t t; pthread_create(&t, NULL, sampler_main, NULL);
}
#else
static void prof_start(void) {}
#endif

/* SCUD_HOST_PGO_SECONDS=n (instrumented builds): write the profile after n seconds and quit */
#include <dlfcn.h>
static void *pgo_thread(void *a) {
    sleep((unsigned)(uintptr_t)a);
    int (*w)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "__llvm_profile_write_file");
    if (w) { w(); log_msg("profile written"); } else log_msg("not an instrumented build");
    _exit(0); return NULL;
}
static void pgo_start(void) {
    const char *e = getenv("SCUD_HOST_PGO_SECONDS"); if (!e) return;
    pthread_t t; pthread_create(&t, NULL, pgo_thread, (void *)(uintptr_t)atoi(e));
}

extern uint8_t *rt_regs(void);
static void crash_handler(int sig, siginfo_t *si, void *uc) {
    (void)uc; uint8_t *R = rt_regs(); uint64_t rsp, ret = 0; memcpy(&rsp, R + 0x20, 8);
    if (rsp > 0x10000) memcpy(&ret, (void *)(uintptr_t)rsp, 8);
    char b[512]; int n = snprintf(b, sizeof b, "\n[host] CRASH signal %d, fault address %p; guest rsp=%#llx [rsp]=%#llx\n", sig, si->si_addr, (unsigned long long)rsp, (unsigned long long)ret);
    if (write(2, b, n) < 0) {}
    void *bt[48]; int k = backtrace(bt, 48); backtrace_symbols_fd(bt, k, 2);
    _exit(139);
}

struct run_args { uint64_t entry; int rc; };
static void *game_thread(void *p) {
    struct run_args *a = p;
    log_msg("entering the translated game at %#llx", (unsigned long long)a->entry);
    a->rc = (int)call_guest(a->entry, 0, 0, 0, 0);
    log_msg("the game returned %d", a->rc);
    exit(a->rc);
    return NULL;
}

/* The image must sit at 0x140000000. Ask for that range before anything else is allocated, without MAP_FIXED (so nothing
 * of ours is overwritten); if ASLR put something there, start over with a fresh random layout. */
static void reserve_image_range(char **argv) {
    void *want = (void *)(uintptr_t)0x140000000ull; size_t len = 0x1500000;
    void *p = mmap(want, len, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == want) return;
    if (p != MAP_FAILED) munmap(p, len);
    const char *t = getenv("SCUD_HOST_RESERVE_TRY"); int n = t ? atoi(t) : 0;
    if (n >= 8) { fprintf(stderr, "[host] address 0x140000000 is not available for the game image\n"); exit(1); }
    char b[16]; snprintf(b, sizeof b, "%d", n + 1); setenv("SCUD_HOST_RESERVE_TRY", b, 1);
    char self[PATH_MAX];
#ifdef __APPLE__
    uint32_t sz = sizeof self; if (_NSGetExecutablePath(self, &sz)) exit(1);
#else
    ssize_t k = readlink("/proc/self/exe", self, sizeof self - 1); if (k <= 0) exit(1); self[k] = 0;
#endif
    execv(self, argv); exit(1);
}

int main(int argc, char **argv) {
    reserve_image_range(argv);
    g_trace = getenv("SCUD_HOST_TRACE") ? atoi(getenv("SCUD_HOST_TRACE")) : 0; if (getenv("SCUD_HOST_TRACE") && g_trace < 1) g_trace = 1;
    { const char *r = getenv("SCUD_RENDER"); setenv("SDL_RENDER_DRIVER", r && *r ? r : "opengl", 1); }
    find_dirs();
    if (chdir(g_game_dir)) { log_msg("cannot chdir to %s: %s", g_game_dir, strerror(errno)); return 1; }
    char winp[1100]; snprintf(winp, sizeof winp, "Z:%s/scudrace.exe", g_game_dir);
    for (char *c = winp; *c; c++) if (*c == '/') *c = '\\';
    snprintf(g_exe_path_win, sizeof g_exe_path_win, "%s", winp);
    log_msg("game folder: %s", g_game_dir);
    { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
      static char altstack[1 << 16]; stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack }; sigaltstack(&ss, NULL);
      sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGFPE, &sa, NULL); }
    { extern void host_stats_start(void); host_stats_start(); }
    prof_start();
    pgo_start();
    extern void host_set_args(int, char **);
    host_set_args(argc, argv);
    static struct run_args a; a.entry = load_image();
#ifdef __APPLE__
    /* Cocoa wants windows and events on the main thread: run the game there (the binary is linked with a 512 MB main stack) */
    game_thread(&a);
#else
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256u << 20);   /* translated frames recurse on the native stack */
    pthread_t th; if (pthread_create(&th, &at, game_thread, &a)) { log_msg("cannot start the game thread"); return 1; }
    pthread_join(th, NULL);
#endif
    return a.rc;
}
