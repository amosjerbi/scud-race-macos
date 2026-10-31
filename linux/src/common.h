/* Scud Race Linux native host: shared declarations.
 *
 * The game is a Windows x86-64 PE. This host is a Linux x86-64 ELF that maps the PE image into its own
 * address space and runs the game's code directly; every Windows API the game imports is implemented here on
 * top of Linux (glibc, pthreads, SDL2). No Wine, no emulator: the CPU runs the game's x86-64 directly.
 */
#ifndef SCUD_COMMON_H
#define SCUD_COMMON_H
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS __attribute__((ms_abi))

typedef void *HANDLE;
typedef int32_t BOOL;
typedef uint32_t DWORD;
typedef uint16_t WCHAR;
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

struct import_entry { const char *name; void *fn; };

/* tables, one per source file; searched by name when the PE's imports are resolved */
extern const struct import_entry sdl_generated_table[];
extern const struct import_entry sdl_special_table[];
extern const struct import_entry k32_table[];
extern const struct import_entry crt_table[];
extern const struct import_entry misc_table[]; /* user32, winhttp, ws2_32, winrt, d3d */

/* loader.c */
extern uintptr_t g_image_base;
void *rev_thunk(void *ms_fn);           /* SysV-callable stub that forwards to a Win64 function (<= 4 int args) */
void log_msg(const char *fmt, ...);
extern int g_trace;
extern char g_game_dir[1024];           /* unix path of the folder holding scud.zip, ui/, Music/ */
extern char g_exe_path_win[1100];       /* what GetModuleFileName reports (Z:\...\scudrace.exe) */

/* fake TEB (the three gs: reads in the image are patched to read here) */
#define FAKE_TEB_ADDR 0x10000000u
struct fake_teb { uint64_t tib[16]; };
extern volatile struct fake_teb *g_teb;

/* utf16 <-> utf8, path translation */
size_t utf16_to_utf8(const WCHAR *w, int wlen, char *out, size_t outcap);   /* wlen<0: NUL-terminated; returns bytes written (no NUL counted) */
size_t utf8_to_utf16(const char *s, int slen, WCHAR *out, size_t outcap);
void win_path_to_unix(const char *in, char *out, size_t cap);
void win_wpath_to_unix(const WCHAR *in, char *out, size_t cap);

/* fibers (fiber.S) */
void *fiber_create(size_t stack, void (*entry)(void *), void *param);
void fiber_switch(void *to);

static inline uint64_t clock_gettime_nsec_np(clockid_t c) { struct timespec t; clock_gettime(c, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
#define CLOCK_MONOTONIC_RAW_ CLOCK_MONOTONIC_RAW
static inline uint32_t host_tid(void) { return (uint32_t)syscall(SYS_gettid); }
#endif
