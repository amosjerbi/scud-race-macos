/* `scudrace-host --selftest`: exercise the Win32/CRT/SDL shims exactly as the PE does (through Win64-ABI function pointers). */
#include "common.h"
#include <unistd.h>
#include <setjmp.h>

extern void *host_lookup(const char *name);
static int g_fail, g_pass;
#define CHECK(cond, what) do { if (cond) { g_pass++; printf("  ok    %s\n", what); } else { g_fail++; printf("  FAIL  %s\n", what); } } while (0)
#define FN(name, type) type name = (type)host_lookup(#name); if (!name) { printf("  FAIL  import %s missing\n", #name); g_fail++; return; }

typedef MS int (*fprintf_t)(void *, const char *, ...);
typedef MS void *(*fopen_t)(const char *, const char *);
typedef MS int (*fclose_t)(void *);
typedef MS size_t (*fread_t)(void *, size_t, size_t, void *);
typedef MS void *(*tmpfile_t)(void);
typedef MS int (*fseek_t)(void *, int32_t, int);

static void test_printf(void) {
    puts("printf (msvcrt semantics)");
    FN(fprintf, fprintf_t) FN(tmpfile, tmpfile_t) FN(fread, fread_t) FN(fseek, fseek_t) FN(fclose, fclose_t)
    void *f = tmpfile();
    fprintf(f, "%ld|%lu|%I64d|%llx|%s|%ls|%5.2f|%c|%08X|%-4d|%hd|%zu|%p", -5L, 4000000000UL, (long long)-1234567890123LL, 0xDEADBEEFCAFEULL, "str",
            (const uint16_t[]){'w', 'i', 'd', 'e', 0}, 3.14159, 'Z', 0xBEEF, 7, (short)-3, (size_t)99, (void *)0x1234);
    fseek(f, 0, 0); char buf[256] = { 0 }; fread(buf, 1, sizeof buf - 1, f); fclose(f);
    CHECK(!strcmp(buf, "-5|4000000000|-1234567890123|deadbeefcafe|str|wide| 3.14|Z|0000BEEF|7   |-3|99|0000000000001234"), "format string result");
    if (strcmp(buf, "-5|4000000000|-1234567890123|deadbeefcafe|str|wide| 3.14|Z|0000BEEF|7   |-3|99|0000000000001234")) printf("        got: %s\n", buf);
}

typedef MS void *(*CreateFiberEx_t)(size_t, size_t, DWORD, MS void (*)(void *), void *);
typedef MS void *(*Convert_t)(void *, DWORD);
typedef MS void (*Switch_t)(void *);
typedef MS int (*IsFiber_t)(void);
static void *g_main_fiber, *g_co; static int g_steps; static Switch_t g_switch;
static MS void co_main(void *p) { (void)p; for (;;) { g_steps++; g_switch(g_main_fiber); } }
static void test_fibers(void) {
    puts("fibers (the game's m3_coro_* coroutines)");
    FN(CreateFiberEx, CreateFiberEx_t) FN(ConvertThreadToFiberEx, Convert_t) FN(SwitchToFiber, Switch_t) FN(IsThreadAFiber, IsFiber_t)
    g_switch = SwitchToFiber;
    CHECK(!IsThreadAFiber(), "thread is not a fiber before conversion");
    g_main_fiber = ConvertThreadToFiberEx((void *)0xABC, 0);
    CHECK(IsThreadAFiber(), "thread is a fiber after conversion");
    extern void *host_current_fiber(void);
    CHECK(host_current_fiber() == g_main_fiber && *(void **)g_main_fiber == (void *)0xABC, "GetCurrentFiber/GetFiberData semantics (param is the first qword)");
    g_co = CreateFiberEx(0, 0, 0, co_main, (void *)0x123);
    volatile double keep = 1.5; /* lives in an xmm register across switches */
    for (int i = 0; i < 100000; i++) SwitchToFiber(g_co);
    CHECK(g_steps == 100000, "100000 fiber round trips");
    CHECK(keep == 1.5, "callee state survives switches");
}

__attribute__((returns_twice, ms_abi)) extern int ms_setjmp(void *buf, void *frame);
extern MS void ms_longjmp(void *buf, int v);
static void test_setjmp(void) {
    puts("setjmp/longjmp (Win64 jmp_buf)");
    static __attribute__((aligned(16))) char buf[256];
    volatile int n = 0;
    int r = ms_setjmp(buf, NULL);
    if (r == 0) { n++; ms_longjmp(buf, 7); }
    CHECK(r == 7 && n == 1, "longjmp returns the requested value to the setjmp site");
}

typedef MS void *(*CreateFileW_t)(const uint16_t *, DWORD, DWORD, void *, DWORD, DWORD, void *);
typedef MS int (*WriteFile_t)(void *, const void *, DWORD, DWORD *, void *);
typedef MS DWORD (*GetFileSize_t)(void *, DWORD *);
typedef MS int (*CloseHandle_t)(void *);
typedef MS DWORD (*GetFileAttributesW_t)(const uint16_t *);
typedef MS int (*DeleteFileW_t)(const uint16_t *);
typedef MS int (*CreateDirectoryW_t)(const uint16_t *, void *);
typedef MS void *(*FindFirstFileW_t)(const uint16_t *, void *);
typedef MS int (*FindNextFileW_t)(void *, void *);
typedef MS int (*FindClose_t)(void *);
static void w(const char *s, uint16_t *out) { size_t n = utf8_to_utf16(s, -1, out, 255); out[n] = 0; }
static void test_files(void) {
    puts("files and Windows paths");
    FN(CreateFileW, CreateFileW_t) FN(WriteFile, WriteFile_t) FN(GetFileSize, GetFileSize_t) FN(CloseHandle, CloseHandle_t)
    FN(GetFileAttributesW, GetFileAttributesW_t) FN(DeleteFileW, DeleteFileW_t) FN(CreateDirectoryW, CreateDirectoryW_t)
    FN(FindFirstFileW, FindFirstFileW_t) FN(FindNextFileW, FindNextFileW_t) FN(FindClose, FindClose_t)
    uint16_t d[256], p[256], pat[256];
    w("selftest_dir", d); w("selftest_dir\\a.txt", p); w("selftest_dir\\*.txt", pat);
    CHECK(CreateDirectoryW(d, NULL), "CreateDirectoryW");
    void *h = CreateFileW(p, 0x40000000, 0, NULL, 2, 0x80, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "CreateFileW with a backslash path");
    DWORD wr = 0; CHECK(WriteFile(h, "hello", 5, &wr, NULL) && wr == 5, "WriteFile");
    CloseHandle(h);
    h = CreateFileW(p, 0x80000000, 0, NULL, 3, 0x80, NULL);
    CHECK(GetFileSize(h, NULL) == 5, "GetFileSize"); CloseHandle(h);
    CHECK(GetFileAttributesW(d) == 0x10 && GetFileAttributesW(p) == 0x80, "GetFileAttributesW (directory / file)");
    uint8_t fd[600] = { 0 }; void *fh = FindFirstFileW(pat, fd); int found = 0;
    if (fh != INVALID_HANDLE_VALUE) { do { if (((uint16_t *)(fd + 44))[0] == 'a') found++; } while (FindNextFileW(fh, fd)); FindClose(fh); }
    CHECK(found == 1, "FindFirstFileW/FindNextFileW with a wildcard");
    CHECK(DeleteFileW(p), "DeleteFileW"); rmdir("selftest_dir");
    char u[256]; win_path_to_unix("C:\\Users\\x\\ui\\a.wav", u, sizeof u); CHECK(!strcmp(u, "/Users/x/ui/a.wav"), "drive-letter path translation");
    win_path_to_unix("NUL", u, sizeof u); CHECK(!strcmp(u, "/dev/null"), "NUL maps to /dev/null");
}

typedef MS void *(*CreateThread_t)(void *, size_t, MS DWORD (*)(void *), void *, DWORD, DWORD *);
typedef MS DWORD (*Wait_t)(void *, DWORD);
typedef MS void (*Cs_t)(void *);
typedef MS int (*SleepCv_t)(void *, void *, DWORD, DWORD);
typedef MS void (*Wake_t)(void *);
static volatile int g_thr_flag; static void *g_srw, *g_cv; static volatile int g_ready;
static MS DWORD thr_fn(void *a) { g_thr_flag = (int)(intptr_t)a; return 42; }
static MS DWORD cv_fn(void *a) {
    (void)a;
    Cs_t acq = host_lookup("AcquireSRWLockExclusive"), rel = host_lookup("ReleaseSRWLockExclusive"); Wake_t wake = host_lookup("WakeConditionVariable");
    acq(&g_srw); g_ready = 1; wake(&g_cv); rel(&g_srw); return 0;
}
static void test_threads(void) {
    puts("threads and locks");
    FN(CreateThread, CreateThread_t) FN(WaitForSingleObject, Wait_t) FN(AcquireSRWLockExclusive, Cs_t) FN(ReleaseSRWLockExclusive, Cs_t) FN(SleepConditionVariableSRW, SleepCv_t)
    void *t = CreateThread(NULL, 0, thr_fn, (void *)77, 0, NULL);
    CHECK(WaitForSingleObject(t, 5000) == 0 && g_thr_flag == 77, "CreateThread + WaitForSingleObject");
    AcquireSRWLockExclusive(&g_srw);
    void *t2 = CreateThread(NULL, 0, cv_fn, NULL, 0, NULL);
    int waited = 0; while (!g_ready && waited++ < 50) SleepConditionVariableSRW(&g_cv, &g_srw, 100, 0);
    ReleaseSRWLockExclusive(&g_srw);
    CHECK(g_ready == 1, "SRW lock + condition variable hand-off between threads"); WaitForSingleObject(t2, 5000);
}

typedef MS uintptr_t (*socket_t)(int, int, int);
typedef MS int (*bind_t)(uintptr_t, const void *, int);
typedef MS int (*sendto_t)(uintptr_t, const void *, int, int, const void *, int);
typedef MS int (*recvfrom_t)(uintptr_t, void *, int, int, void *, int *);
typedef MS int (*select_t)(int, void *, void *, void *, const int32_t *);
typedef MS int (*getsockname_dummy_t)(void);
typedef MS int (*closesocket_t)(uintptr_t);
typedef MS int (*getaddrinfo_t)(const char *, const char *, const void *, void **);
typedef MS void (*freeaddrinfo_t)(void *);
typedef MS int (*WSAStartup_t)(uint16_t, void *);
static void test_sockets(void) {
    puts("winsock translation");
    FN(socket, socket_t) FN(bind, bind_t) FN(sendto, sendto_t) FN(recvfrom, recvfrom_t) FN(select, select_t) FN(closesocket, closesocket_t)
    FN(getaddrinfo, getaddrinfo_t) FN(freeaddrinfo, freeaddrinfo_t) FN(WSAStartup, WSAStartup_t)
    uint8_t wsa[400]; CHECK(WSAStartup(0x0202, wsa) == 0, "WSAStartup");
    uintptr_t s = socket(2, 2, 17); CHECK(s != (uintptr_t)-1, "socket(AF_INET, SOCK_DGRAM)");
    uint8_t sa[16] = { 2, 0, 0x30, 0x39 /* port 12345 */, 127, 0, 0, 1 };      /* Windows sockaddr_in: family is a u16 */
    CHECK(bind(s, sa, 16) == 0, "bind with a Windows sockaddr");
    CHECK(sendto(s, "ping", 4, 0, sa, 16) == 4, "sendto loopback");
    struct { uint32_t count, pad; uintptr_t arr[64]; } rs = { 1, 0, { s } }; int32_t tv[2] = { 1, 0 };
    CHECK(select(0, &rs, NULL, NULL, tv) == 1 && rs.count == 1, "select with a Windows fd_set");
    char buf[16] = { 0 }; uint8_t from[16] = { 0 }; int fl = 16;
    CHECK(recvfrom(s, buf, 15, 0, from, &fl) == 4 && !strcmp(buf, "ping") && from[0] == 2 && from[1] == 0 && from[4] == 127, "recvfrom returns a Windows sockaddr");
    closesocket(s);
    void *res = NULL; int rc = getaddrinfo("localhost", "80", NULL, &res);
    struct { int flags, family, socktype, protocol; size_t addrlen; char *canon; void *addr; void *next; } *ai = res;
    CHECK(rc == 0 && ai && ai->addrlen >= 16 && (((uint8_t *)ai->addr)[0] == 2 || ((uint8_t *)ai->addr)[0] == 23), "getaddrinfo yields Windows-layout results");
    if (res) freeaddrinfo(res);
}

typedef MS int (*SDL_Init_t)(uint32_t);
typedef MS void *(*SDL_CreateWindow_t)(const char *, int, int, int, int, uint32_t);
typedef MS void *(*SDL_CreateRenderer_t)(void *, int, uint32_t);
typedef MS void *(*SDL_CreateTexture_t)(void *, uint32_t, int, int, int);
typedef MS int (*SDL_UpdateTexture_t)(void *, const void *, const void *, int);
typedef MS int (*SDL_RenderCopy_t)(void *, void *, const void *, const void *);
typedef MS void (*SDL_RenderPresent_t)(void *);
typedef MS int (*SDL_RenderClear_t)(void *);
typedef MS int (*SDL_SetRenderDrawColor_t)(void *, uint8_t, uint8_t, uint8_t, uint8_t);
typedef MS int (*SDL_PollEvent_t)(void *);
typedef MS int (*SDL_NumJoysticks_t)(void);
typedef MS int (*SDL_GetWindowWMInfo_t)(void *, void *);
typedef MS void (*SDL_Delay_t)(uint32_t);
typedef MS void (*SDL_DestroyWindow_t)(void *);
typedef MS uint32_t (*SDL_OpenAudioDevice_t)(const char *, int, const void *, void *, int);
typedef MS int (*SDL_QueueAudio_t)(uint32_t, const void *, uint32_t);
typedef MS void (*SDL_CloseAudioDevice_t)(uint32_t);
typedef MS char *(*SDL_GetBasePath_t)(void);
typedef MS int (*EnumDisplaySettingsA_t)(const char *, uint32_t, uint8_t *);
static void test_sdl(void) {
    puts("SDL window / renderer / audio (the game's platform_init fallback path)");
    FN(SDL_Init, SDL_Init_t) FN(SDL_CreateWindow, SDL_CreateWindow_t) FN(SDL_CreateRenderer, SDL_CreateRenderer_t) FN(SDL_CreateTexture, SDL_CreateTexture_t)
    FN(SDL_UpdateTexture, SDL_UpdateTexture_t) FN(SDL_RenderCopy, SDL_RenderCopy_t) FN(SDL_RenderPresent, SDL_RenderPresent_t) FN(SDL_RenderClear, SDL_RenderClear_t)
    FN(SDL_SetRenderDrawColor, SDL_SetRenderDrawColor_t) FN(SDL_PollEvent, SDL_PollEvent_t) FN(SDL_NumJoysticks, SDL_NumJoysticks_t) FN(SDL_GetWindowWMInfo, SDL_GetWindowWMInfo_t)
    FN(SDL_Delay, SDL_Delay_t) FN(SDL_DestroyWindow, SDL_DestroyWindow_t) FN(SDL_OpenAudioDevice, SDL_OpenAudioDevice_t) FN(SDL_QueueAudio, SDL_QueueAudio_t)
    FN(SDL_CloseAudioDevice, SDL_CloseAudioDevice_t) FN(SDL_GetBasePath, SDL_GetBasePath_t) FN(EnumDisplaySettingsA, EnumDisplaySettingsA_t)
    CHECK(SDL_Init(0x20 | 0x10 | 0x200 | 0x2000 /* video|audio... as the game's mix */) == 0 || SDL_Init(0x20) == 0, "SDL_Init through the Win64 wrapper");
    void *win = SDL_CreateWindow("Scud Race (host selftest)", 0x2FFF0000, 0x2FFF0000, 640, 480, 0x20);
    CHECK(win != NULL, "SDL_CreateWindow with 6 arguments (two on the stack)");
    uint8_t wm[64] = { 0 }; CHECK(SDL_GetWindowWMInfo(win, wm) == 0, "SDL_GetWindowWMInfo reports no Win32 window (D3D path is skipped)");
    void *ren = SDL_CreateRenderer(win, -1, 2); CHECK(ren != NULL, "SDL_CreateRenderer (accelerated -> Metal)");
    void *tex = SDL_CreateTexture(ren, 0x16362004 /* ARGB8888 */, 1 /* streaming */, 320, 240); CHECK(tex != NULL, "SDL_CreateTexture ARGB8888 streaming");
    static uint32_t px[320 * 240];
    int ok = 1;
    for (int f = 0; f < 45; f++) {
        for (int i = 0; i < 320 * 240; i++) px[i] = 0xFF000000u | ((i + f * 4) & 0xFF) << 16 | (f * 5 & 0xFF) << 8 | 0x40;
        ok &= SDL_UpdateTexture(tex, NULL, px, 320 * 4) == 0;
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
        ok &= SDL_RenderCopy(ren, tex, NULL, NULL) == 0; SDL_RenderPresent(ren);
        uint8_t ev[64]; while (SDL_PollEvent(ev)) {}
        SDL_Delay(8);
    }
    CHECK(ok, "45 frames: UpdateTexture -> RenderCopy -> RenderPresent");
    int nj = SDL_NumJoysticks(); printf("  info  joysticks seen: %d\n", nj);
    struct { int freq; uint16_t fmt; uint8_t ch, sil; uint16_t samples, pad; uint32_t size; void *cb, *ud; } want = { 48000, 0x8010 /* S16 */, 2, 0, 1024, 0, 0, NULL, NULL }, have;
    uint32_t dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    CHECK(dev != 0, "SDL_OpenAudioDevice (queue mode, struct passed by pointer)");
    if (dev) { static int16_t silence[4800 * 2]; CHECK(SDL_QueueAudio(dev, silence, sizeof silence) == 0, "SDL_QueueAudio"); SDL_CloseAudioDevice(dev); }
    char *bp = SDL_GetBasePath(); CHECK(bp && bp[strlen(bp) - 1] == '/', "SDL_GetBasePath returns the game folder"); if (bp) printf("  info  base path: %s\n", bp);
    uint8_t dm[160]; CHECK(EnumDisplaySettingsA(NULL, 0xFFFFFFFFu, dm) && *(uint32_t *)(dm + 108) > 0, "EnumDisplaySettingsA reports the desktop mode");
    printf("  info  desktop mode %ux%u @ %uHz\n", *(uint32_t *)(dm + 108), *(uint32_t *)(dm + 112), *(uint32_t *)(dm + 120));
    SDL_DestroyWindow(win);
}

int host_selftest(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_printf(); test_setjmp(); test_fibers(); test_files(); test_threads(); test_sockets(); test_sdl();
    printf("\nselftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
