/* user32, winhttp, ws2_32, winrt, d3d9/d3d11 entry points, and the SDL calls that need more than a typed wrapper. */
#include "common.h"
#include <SDL.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

/* ------------------------------------------------------------------ d3d: report "no device" so platform_init falls back to SDL_Renderer */
static MS void *Direct3DCreate9(unsigned v) { (void)v; return NULL; }
static MS int32_t D3D11CreateDeviceAndSwapChain(void) { return (int32_t)0x80004005; /* E_FAIL */ }

/* ------------------------------------------------------------------ winrt (only the Windows Gaming Input rumble worker uses it) */
static MS int32_t RoInitialize(int t) { (void)t; return 0; }
static MS void RoUninitialize(void) {}
static MS int32_t RoGetActivationFactory(void *cls, const void *iid, void **out) { (void)cls; (void)iid; *out = NULL; return (int32_t)0x80040154; /* REGDB_E_CLASSNOTREG */ }
static MS int32_t WindowsCreateStringReference(const void *s, unsigned len, void *hdr, void **out) { (void)s; (void)len; *out = hdr; return 0; }

/* ------------------------------------------------------------------ winhttp: no network updater on this port */
static MS void *WinHttpOpen(const void *a, unsigned b, const void *c, const void *d, unsigned e) { (void)a;(void)b;(void)c;(void)d;(void)e; return NULL; }
static MS int WinHttpFail(void) { return 0; }

/* ------------------------------------------------------------------ user32 */
static MS void *CreateMenu(void) { return (void *)1; }
static MS void *CreatePopupMenu(void) { return (void *)1; }
static MS int AppendMenuA(void *m, unsigned f, uintptr_t id, const char *s) { (void)m;(void)f;(void)id;(void)s; return 1; }
static MS int DestroyMenu(void *m) { (void)m; return 1; }
static MS void *GetMenu(void *w) { (void)w; return NULL; }
static MS int SetMenu(void *w, void *m) { (void)w; (void)m; return 1; }
static MS intptr_t DialogBoxIndirectParamW(void *i, const void *t, void *p, void *proc, intptr_t param) { (void)i;(void)t;(void)p;(void)proc;(void)param; return 0; }
static MS int EndDialog(void *d, intptr_t r) { (void)d; (void)r; return 1; }
static MS unsigned GetDlgItemTextW(void *d, int id, uint16_t *buf, int n) { (void)d; (void)id; if (n > 0) buf[0] = 0; return 0; }
static MS int SetDlgItemTextW(void *d, int id, const uint16_t *s) { (void)d; (void)id; (void)s; return 1; }
static MS int GetClientRect(void *w, int *r) { (void)w; (void)r; return 0; }
static MS int GetWindowRect(void *w, int *r) { (void)w; (void)r; return 0; }
static MS int MessageBoxA(void *w, const char *text, const char *cap, unsigned type) {
    (void)w; (void)type; fprintf(stderr, "[game] %s: %s\n", cap ? cap : "", text ? text : "");
    if (!getenv("SCUD_HOST_NOGUI")) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, cap ? cap : "Scud Race", text ? text : "", NULL);
    return 1;
}
static MS int EnumDisplaySettingsA(const char *dev, uint32_t mode, uint8_t *dm) {
    (void)dev; SDL_DisplayMode m;
    int ok = (mode == 0xFFFFFFFFu || mode == 0xFFFFFFFEu) ? SDL_GetDesktopDisplayMode(0, &m) : SDL_GetDisplayMode(0, (int)mode, &m);
    if (ok != 0) return 0;
    memset(dm, 0, 156);
    *(uint16_t *)(dm + 36) = 156;
    *(uint32_t *)(dm + 40) = 0x5C0000 /* BITSPERPEL|PELSWIDTH|PELSHEIGHT|DISPLAYFREQUENCY */;
    *(uint32_t *)(dm + 104) = 32; *(uint32_t *)(dm + 108) = m.w; *(uint32_t *)(dm + 112) = m.h; *(uint32_t *)(dm + 120) = m.refresh_rate ? m.refresh_rate : 60;
    return 1;
}

/* ------------------------------------------------------------------ winsock on BSD sockets */
typedef uintptr_t SOCKET;
static _Thread_local int g_wsaerr;
static int wsa_from_errno(int e) {
    switch (e) { case EWOULDBLOCK: return 10035; case EINPROGRESS: return 10036; case EALREADY: return 10037; case ENOTSOCK: return 10038;
                 case EMSGSIZE: return 10040; case EADDRINUSE: return 10048; case EADDRNOTAVAIL: return 10049; case ENETUNREACH: return 10051;
                 case ECONNABORTED: return 10053; case ECONNRESET: return 10054; case ENOBUFS: return 10055; case EISCONN: return 10056;
                 case ENOTCONN: return 10057; case ETIMEDOUT: return 10060; case ECONNREFUSED: return 10061; case EHOSTUNREACH: return 10065;
                 case EINTR: return 10004; case EBADF: return 10009; case EACCES: return 10013; case EINVAL: return 10022; default: return 10000 + e; }
}
#define WSAFAIL() (g_wsaerr = wsa_from_errno(errno), -1)
static int af_in(int a) { return a == 23 ? AF_INET6 : a; }
static int af_out(int a) { return a == AF_INET6 ? 23 : a; }
/* Windows sockaddr: u16 family at 0. BSD: u8 len, u8 family. */
static socklen_t sa_in(const void *w, int len, struct sockaddr_storage *o) {
    memset(o, 0, sizeof *o); memcpy(o, w, len < (int)sizeof *o ? len : (int)sizeof *o);
    uint16_t fam = *(const uint16_t *)w; ((struct sockaddr *)o)->sa_family = af_in(fam);
    socklen_t l = ((struct sockaddr *)o)->sa_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    return l;
}
static void sa_out(const struct sockaddr *b, void *w, int *wlen) {
    uint8_t tmp[128]; socklen_t l = b->sa_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    memcpy(tmp, b, l); *(uint16_t *)tmp = af_out(b->sa_family);
    if (wlen) { if (*wlen > (int)l) *wlen = l; memcpy(w, tmp, *wlen); } else memcpy(w, tmp, l);
}
static MS int WSAStartup(uint16_t v, void *data) { memset(data, 0, 400); *(uint16_t *)data = v; *((uint16_t *)data + 1) = v; return 0; }
static MS int WSAGetLastError(void) { return g_wsaerr; }
static MS SOCKET socket_ms(int af, int type, int proto) {
    int fd = socket(af_in(af), type, proto); if (fd < 0) { WSAFAIL(); return (SOCKET)-1; }
    signal(SIGPIPE, SIG_IGN); return (SOCKET)fd;
}
static MS int closesocket(SOCKET s) { return close((int)s); }
static MS int bind_ms(SOCKET s, const void *a, int len) { struct sockaddr_storage b; socklen_t l = sa_in(a, len, &b); return bind((int)s, (void *)&b, l) ? WSAFAIL() : 0; }
static MS int connect_ms(SOCKET s, const void *a, int len) { struct sockaddr_storage b; socklen_t l = sa_in(a, len, &b); return connect((int)s, (void *)&b, l) ? WSAFAIL() : 0; }
static MS int listen_ms(SOCKET s, int bl) { return listen((int)s, bl) ? WSAFAIL() : 0; }
static MS SOCKET accept_ms(SOCKET s, void *a, int *len) {
    struct sockaddr_storage b; socklen_t l = sizeof b; int fd = accept((int)s, (void *)&b, &l);
    if (fd < 0) { WSAFAIL(); return (SOCKET)-1; }
    if (a) sa_out((void *)&b, a, len);
    signal(SIGPIPE, SIG_IGN); return (SOCKET)fd;
}
static MS int send_ms(SOCKET s, const void *b, int n, int fl) { int r = (int)send((int)s, b, n, fl); return r < 0 ? WSAFAIL() : r; }
static MS int recv_ms(SOCKET s, void *b, int n, int fl) { int r = (int)recv((int)s, b, n, fl); return r < 0 ? WSAFAIL() : r; }
static MS int sendto_ms(SOCKET s, const void *b, int n, int fl, const void *a, int len) {
    struct sockaddr_storage d; socklen_t l = sa_in(a, len, &d); int r = (int)sendto((int)s, b, n, fl, (void *)&d, l); return r < 0 ? WSAFAIL() : r;
}
static MS int recvfrom_ms(SOCKET s, void *b, int n, int fl, void *a, int *len) {
    struct sockaddr_storage d; socklen_t l = sizeof d; int r = (int)recvfrom((int)s, b, n, fl, (void *)&d, &l);
    if (r < 0) return WSAFAIL();
    if (a && len) sa_out((void *)&d, a, len);
    return r;
}
static MS int ioctlsocket(SOCKET s, long cmd, unsigned long *argp) {
    (void)cmd; int fl = fcntl((int)s, F_GETFL, 0);
    if (cmd == (long)0x8004667E /* FIONBIO */) { fcntl((int)s, F_SETFL, *(uint32_t *)argp ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)); return 0; }
    g_wsaerr = 10022; return -1;
}
static int so_in(int opt) {      /* Winsock SOL_SOCKET option numbers -> Linux */
    switch (opt) { case 0x0004: return SO_REUSEADDR; case 0x0008: return SO_KEEPALIVE; case 0x0020: return SO_BROADCAST; case 0x0080: return SO_LINGER;
                   case 0x1001: return SO_SNDBUF; case 0x1002: return SO_RCVBUF; case 0x1005: return SO_SNDTIMEO; case 0x1006: return SO_RCVTIMEO; }
    return opt;
}
static MS int setsockopt_ms(SOCKET s, int level, int opt, const char *val, int len) {
    if (level != 0xFFFF) return setsockopt((int)s, level, opt, val, len) ? WSAFAIL() : 0;
    int lo = so_in(opt);
    if ((lo == SO_RCVTIMEO || lo == SO_SNDTIMEO) && len >= 4) {                          /* DWORD ms -> timeval */
        uint32_t ms = *(const uint32_t *)val; struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
        return setsockopt((int)s, SOL_SOCKET, lo, &tv, sizeof tv) ? WSAFAIL() : 0;
    }
    if (lo == SO_LINGER && len >= 4) {                                                   /* u_short pair -> int pair */
        struct linger lg = { ((const uint16_t *)val)[0], ((const uint16_t *)val)[1] };
        return setsockopt((int)s, SOL_SOCKET, SO_LINGER, &lg, sizeof lg) ? WSAFAIL() : 0;
    }
    return setsockopt((int)s, SOL_SOCKET, lo, val, len) ? WSAFAIL() : 0;
}
static MS uint32_t htonl_ms(uint32_t v) { return htonl(v); }
static MS uint32_t ntohl_ms(uint32_t v) { return ntohl(v); }
static MS uint16_t htons_ms(uint16_t v) { return htons(v); }
static MS uint16_t ntohs_ms(uint16_t v) { return ntohs(v); }

struct win_fdset { uint32_t count; uint32_t pad; SOCKET arr[64]; };
static int to_bsd(const struct win_fdset *w, fd_set *o, int *mx) {
    FD_ZERO(o); if (!w) return 0;
    for (uint32_t i = 0; i < w->count; i++) { FD_SET((int)w->arr[i], o); if ((int)w->arr[i] > *mx) *mx = (int)w->arr[i]; }
    return 1;
}
static void from_bsd(struct win_fdset *w, const fd_set *o) {
    if (!w) return; uint32_t n = 0;
    for (uint32_t i = 0; i < w->count; i++) if (FD_ISSET((int)w->arr[i], o)) w->arr[n++] = w->arr[i];
    w->count = n;
}
static MS int select_ms(int nfds, struct win_fdset *r, struct win_fdset *w, struct win_fdset *e, const int32_t *tv) {
    (void)nfds; fd_set fr, fw, fe; int mx = -1;
    int hr = to_bsd(r, &fr, &mx), hw = to_bsd(w, &fw, &mx), he = to_bsd(e, &fe, &mx);
    struct timeval t, *pt = NULL; if (tv) { t.tv_sec = tv[0]; t.tv_usec = tv[1]; pt = &t; }
    int n = select(mx + 1, hr ? &fr : NULL, hw ? &fw : NULL, he ? &fe : NULL, pt);
    if (n < 0) return WSAFAIL();
    if (hr) from_bsd(r, &fr);
    if (hw) from_bsd(w, &fw);
    if (he) from_bsd(e, &fe);
    return n;
}
/* ADDRINFOA as the game sees it: {int flags,family,socktype,protocol; size_t addrlen; char *canon; sockaddr *addr; next} */
struct win_addrinfo { int flags, family, socktype, protocol; size_t addrlen; char *canon; void *addr; struct win_addrinfo *next; };
static MS int getaddrinfo_ms(const char *node, const char *svc, const struct win_addrinfo *h, struct win_addrinfo **res) {
    struct addrinfo hints, *r0 = NULL; memset(&hints, 0, sizeof hints);
    if (h) { hints.ai_flags = h->flags & 7; hints.ai_family = af_in(h->family); hints.ai_socktype = h->socktype; hints.ai_protocol = h->protocol; }
    int rc = getaddrinfo(node, svc, h ? &hints : NULL, &r0);
    if (rc) { g_wsaerr = rc == EAI_NONAME ? 11001 : 11002; return g_wsaerr; }
    struct win_addrinfo *head = NULL, **tail = &head;
    for (struct addrinfo *a = r0; a; a = a->ai_next) {
        struct win_addrinfo *w = calloc(1, sizeof *w);
        w->family = af_out(a->ai_family); w->socktype = a->ai_socktype; w->protocol = a->ai_protocol; w->addrlen = a->ai_addrlen;
        w->addr = calloc(1, 128); sa_out(a->ai_addr, w->addr, NULL);
        *tail = w; tail = &w->next;
    }
    freeaddrinfo(r0); *res = head; return 0;
}
static MS void freeaddrinfo_ms(struct win_addrinfo *a) { while (a) { struct win_addrinfo *n = a->next; free(a->addr); free(a); a = n; } }

const struct import_entry misc_table[] = {
    { "Direct3DCreate9", Direct3DCreate9 }, { "D3D11CreateDeviceAndSwapChain", D3D11CreateDeviceAndSwapChain },
    { "RoInitialize", RoInitialize }, { "RoUninitialize", RoUninitialize }, { "RoGetActivationFactory", RoGetActivationFactory },
    { "WindowsCreateStringReference", WindowsCreateStringReference },
    { "WinHttpOpen", WinHttpOpen }, { "WinHttpCloseHandle", WinHttpFail }, { "WinHttpConnect", WinHttpFail }, { "WinHttpCrackUrl", WinHttpFail },
    { "WinHttpOpenRequest", WinHttpFail }, { "WinHttpQueryHeaders", WinHttpFail }, { "WinHttpReadData", WinHttpFail }, { "WinHttpReceiveResponse", WinHttpFail },
    { "WinHttpSendRequest", WinHttpFail }, { "WinHttpSetTimeouts", WinHttpFail },
    { "AppendMenuA", AppendMenuA }, { "CreateMenu", CreateMenu }, { "CreatePopupMenu", CreatePopupMenu }, { "DestroyMenu", DestroyMenu },
    { "DialogBoxIndirectParamW", DialogBoxIndirectParamW }, { "EndDialog", EndDialog }, { "EnumDisplaySettingsA", EnumDisplaySettingsA },
    { "GetClientRect", GetClientRect }, { "GetDlgItemTextW", GetDlgItemTextW }, { "GetMenu", GetMenu }, { "GetWindowRect", GetWindowRect },
    { "MessageBoxA", MessageBoxA }, { "SetDlgItemTextW", SetDlgItemTextW }, { "SetMenu", SetMenu },
    { "WSAGetLastError", WSAGetLastError }, { "WSAStartup", WSAStartup }, { "accept", accept_ms }, { "bind", bind_ms }, { "closesocket", closesocket },
    { "connect", connect_ms }, { "freeaddrinfo", freeaddrinfo_ms }, { "getaddrinfo", getaddrinfo_ms }, { "htonl", htonl_ms }, { "htons", htons_ms },
    { "ioctlsocket", ioctlsocket }, { "listen", listen_ms }, { "ntohl", ntohl_ms }, { "ntohs", ntohs_ms }, { "recv", recv_ms }, { "recvfrom", recvfrom_ms },
    { "select", select_ms }, { "send", send_ms }, { "sendto", sendto_ms }, { "setsockopt", setsockopt_ms }, { "socket", socket_ms },
    { 0, 0 }
};

/* ------------------------------------------------------------------ SDL calls that need hand-written wrappers */
static MS SDL_bool w_SDL_GetWindowWMInfo(SDL_Window *w, void *info) { (void)w; (void)info; SDL_SetError("no Win32 window on macOS"); return SDL_FALSE; }
static MS char *w_SDL_GetBasePath(void) {
    size_t n = strlen(g_game_dir); char *p = SDL_malloc(n + 2); memcpy(p, g_game_dir, n); p[n] = '/'; p[n + 1] = 0; return p;
}
static MS SDL_RWops *w_SDL_RWFromFile(const char *file, const char *mode) { char p[2048]; win_path_to_unix(file, p, sizeof p); return SDL_RWFromFile(p, mode); }
static MS void w_SDL_RenderPresent(SDL_Renderer *r) {
    static int on = -1; static Uint64 t0, n;
    if (on < 0) on = getenv("SCUD_HOST_FPS") != NULL;
    SDL_RenderPresent(r);
    if (on) { Uint64 now = SDL_GetTicks64(); if (!t0) t0 = now; n++; if (now - t0 >= 1000) { fprintf(stderr, "[fps] presented %.1f frames/s\n", n * 1000.0 / (now - t0)); t0 = now; n = 0; } }
}
static MS SDL_Window *w_SDL_CreateWindow(const char *t, int x, int y, int w, int h, Uint32 flags) { SDL_Window *r = SDL_CreateWindow(t, x, y, w, h, flags); return r; }
static MS SDL_Renderer *w_SDL_CreateRenderer(SDL_Window *win, int idx, Uint32 flags) {
    SDL_Renderer *r = SDL_CreateRenderer(win, idx, flags); SDL_RendererInfo ri;
    if (!r) { fprintf(stderr, "[host] SDL renderer failed (%s); falling back to software\n", SDL_GetError()); SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"); r = SDL_CreateRenderer(win, idx, flags & ~SDL_RENDERER_ACCELERATED); }
    if (r && !SDL_GetRendererInfo(r, &ri)) fprintf(stderr, "[host] SDL renderer: %s\n", ri.name);
    return r;
}
static MS SDL_AudioDeviceID w_SDL_OpenAudioDevice(const char *dev, int cap, const SDL_AudioSpec *want, SDL_AudioSpec *have, int allowed) {
    SDL_AudioSpec d = *want; SDL_AudioCallback ms_cb = want->callback;
    d.callback = (SDL_AudioCallback)rev_thunk((void *)ms_cb);
    SDL_AudioDeviceID id = SDL_OpenAudioDevice(dev, cap, &d, have, allowed);
    if (have) have->callback = ms_cb;
    return id;
}
static MS int w_SDL_JoystickAttachVirtualEx(const SDL_VirtualJoystickDesc *desc) {
    SDL_VirtualJoystickDesc d = *desc;
    d.Update = (void (*)(void *))rev_thunk((void *)desc->Update);
    d.SetPlayerIndex = (void (*)(void *, int))rev_thunk((void *)desc->SetPlayerIndex);
    d.Rumble = (int (*)(void *, Uint16, Uint16))rev_thunk((void *)desc->Rumble);
    d.RumbleTriggers = (int (*)(void *, Uint16, Uint16))rev_thunk((void *)desc->RumbleTriggers);
    d.SetLED = (int (*)(void *, Uint8, Uint8, Uint8))rev_thunk((void *)desc->SetLED);
    d.SendEffect = (int (*)(void *, const void *, int))rev_thunk((void *)desc->SendEffect);
    return SDL_JoystickAttachVirtualEx(&d);
}
const struct import_entry sdl_special_table[] = {
    { "SDL_GetWindowWMInfo", w_SDL_GetWindowWMInfo }, { "SDL_GetBasePath", w_SDL_GetBasePath }, { "SDL_RWFromFile", w_SDL_RWFromFile },
    { "SDL_CreateWindow", w_SDL_CreateWindow }, { "SDL_CreateRenderer", w_SDL_CreateRenderer }, { "SDL_RenderPresent", w_SDL_RenderPresent }, { "SDL_OpenAudioDevice", w_SDL_OpenAudioDevice },
    { "SDL_JoystickAttachVirtualEx", w_SDL_JoystickAttachVirtualEx },
    { 0, 0 }
};
