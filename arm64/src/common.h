/* Scud Race arm64 build (statically recompiled game): shared declarations for the host shims.
 *
 * The game is a Windows x86-64 PE. This host is a macOS x86-64 Mach-O that maps the PE image into its own
 * address space and runs the game's code directly; every Windows API the game imports is implemented here on
 * top of macOS (libc, pthreads, SDL2). No Wine, no emulator: Rosetta 2 only translates the x86-64 itself.
 */
#ifndef SCUD_COMMON_H
#define SCUD_COMMON_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#ifdef __APPLE__
static inline uint32_t host_tid(void) { return (uint32_t)pthread_mach_thread_np(pthread_self()); }
#else
#include <unistd.h>
#include <sys/syscall.h>
static inline uint64_t clock_gettime_nsec_np(clockid_t c) { struct timespec t; clock_gettime(c, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static inline uint32_t host_tid(void) { return (uint32_t)syscall(SYS_gettid); }
#endif

#define MS   /* arm64: the game's code is lifted C; the shims are plain C functions called through generated thunks */

typedef void *HANDLE;
typedef int32_t BOOL;
typedef uint32_t DWORD;
typedef uint16_t WCHAR;
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

struct import_entry { const char *name; void *fn; const char *sig; };   /* sig: see gen/gen_tabs.py; NULL = data */

/* tables, one per source file; searched by name when the PE's imports are resolved */
extern const struct import_entry sdl_generated_table[];
extern const struct import_entry sdl_special_table[];
extern const struct import_entry k32_table[];
extern const struct import_entry crt_table[];
extern const struct import_entry misc_table[]; /* user32, winhttp, ws2_32, winrt, d3d */

/* loader.c */
extern uintptr_t g_image_base;
void *rev_thunk(void *guest_fn);        /* native stub that forwards to a guest function (<= 4 integer args) */
void log_msg(const char *fmt, ...);
extern int g_trace;
extern char g_game_dir[1024];           /* unix path of the folder holding scud.zip, ui/, Music/ */
extern char g_exe_path_win[1100];       /* what GetModuleFileName reports (Z:\...\scudrace.exe) */

/* guest (lifted x86-64) runtime, rtlift.c */
uint64_t call_guest(uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
typedef uint64_t gfn;                   /* a guest code address */
typedef struct { uint64_t *p; } ms_va_list;
#define MS_VA_ARG(ap, T) ({ T _v; uint64_t _s = *(ap).p++; memcpy(&_v, &_s, sizeof _v); _v; })

/* utf16 <-> utf8, path translation */
size_t utf16_to_utf8(const WCHAR *w, int wlen, char *out, size_t outcap);   /* wlen<0: NUL-terminated; returns bytes written (no NUL counted) */
size_t utf8_to_utf16(const char *s, int slen, WCHAR *out, size_t outcap);
void win_path_to_unix(const char *in, char *out, size_t cap);
void win_wpath_to_unix(const WCHAR *in, char *out, size_t cap);


#endif
