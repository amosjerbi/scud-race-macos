/* Runtime for the C produced by gen_lift.py: types, memory/float helpers, SSE userops, and the hooks the host provides. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdalign.h>
#include <stdlib.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef unsigned __int128 u128;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64; typedef __int128 s128;
typedef u8 __attribute__((aligned(1))) RT1; typedef u16 __attribute__((aligned(1))) RT2; typedef u32 __attribute__((aligned(1))) RT4;
typedef u64 __attribute__((aligned(1))) RT8; typedef u128 __attribute__((aligned(1))) RT16; typedef struct { u8 b[10]; } RT10;

#define LD(T, a) ({ T _v; __builtin_memcpy(&_v, (const void *)(uintptr_t)(a), sizeof _v); _v; })
#define ST(T, a, v) do { T _v = (T)(v); __builtin_memcpy((void *)(uintptr_t)(a), &_v, sizeof _v); } while (0)
static inline float F4(u32 x) { float f; __builtin_memcpy(&f, &x, 4); return f; }
static inline double F8(u64 x) { double f; __builtin_memcpy(&f, &x, 8); return f; }
static inline u32 B4(float f) { u32 x; __builtin_memcpy(&x, &f, 4); return x; }
static inline u64 B8(double f) { u64 x; __builtin_memcpy(&x, &f, 8); return x; }

/* x87 80-bit values, computed in double (bit manipulation on the common paths; libm only for the extremes) */
static inline double P2(int k) { u64 b = (u64)(k + 1023) << 52; double d; __builtin_memcpy(&d, &b, 8); return d; }   /* 2^k, -1022 <= k <= 1023 */
static inline double F80D(const void *p) {
    u64 m; u16 se; __builtin_memcpy(&m, p, 8); __builtin_memcpy(&se, (const u8 *)p + 8, 2);
    int e = se & 0x7fff; double r;
    int k = e - 16383 - 63;
    if (__builtin_expect(e != 0 && e != 0x7fff && k >= -1022 && k <= 1023, 1)) r = (double)m * P2(k);
    else if (e == 0) r = ldexp((double)m, -16382 - 63);
    else if (e == 0x7fff) r = (m << 1) ? NAN : INFINITY;
    else r = ldexp((double)m, k);
    return (se >> 15) ? -r : r;
}
static inline void DF80(void *p, double d) {
    u64 b; __builtin_memcpy(&b, &d, 8);
    u64 m = 0; u16 se = (u16)((b >> 63) << 15); int ed = (int)((b >> 52) & 0x7ff);
    if (__builtin_expect(ed != 0 && ed != 0x7ff, 1)) { m = (1ull << 63) | ((b & 0xfffffffffffffull) << 11); se |= (u16)(ed - 1023 + 16383); }
    else if (ed == 0x7ff) { m = (b & 0xfffffffffffffull) ? 0xC000000000000000ull : 0x8000000000000000ull; se |= 0x7fff; }
    else if (b << 1) { u64 f = b & 0xfffffffffffffull; int z = __builtin_clzll(f); m = f << z; se |= (u16)(16383 + 63 - 1074 - z); }   /* denormal double: f * 2^-1074 */
    __builtin_memcpy(p, &m, 8); __builtin_memcpy((u8 *)p + 8, &se, 2);
}
static inline double U_fcos(double x) { return cos(x); }
static inline double U_fsin(double x) { return sin(x); }
static inline double U_f2xm1(double x) { return exp2(x) - 1.0; }
static inline double U_log2(double x) { return log2(x); }
static inline double U_fpatan(double y, double x) { return atan2(y, x); }
static inline double U_fscale(double a, double b) { return ldexp(a, (int)trunc(b)); }

/* float -> int conversions with x86's "integer indefinite" result */
static inline u32 TRUNC4(double x) { if (x != x || x >= 2147483648.0 || x < -2147483648.0) return 0x80000000u; return (u32)(s32)x; }
static inline u64 TRUNC8(double x) { if (x != x || x >= 9223372036854775808.0 || x < -9223372036854775808.0) return 0x8000000000000000ull; return (u64)(s64)x; }
static inline u16 TRUNC2(double x) { if (x != x || x >= 32768.0 || x < -32768.0) return 0x8000u; return (u16)(s16)x; }

static inline RT10 TRUNC10(double x) { RT10 r; s64 v = (x != x || x >= 9223372036854775808.0 || x < -9223372036854775808.0) ? INT64_MIN : (s64)x; u16 hi = v < 0 ? 0xFFFF : 0; __builtin_memcpy(r.b, &v, 8); __builtin_memcpy(r.b + 8, &hi, 2); return r; }

/* SSE ops the p-code leaves as user ops */
typedef float v4f __attribute__((vector_size(16))); typedef double v2d __attribute__((vector_size(16)));
static inline v4f asv4f(u128 x) { v4f v; __builtin_memcpy(&v, &x, 16); return v; }
static inline u128 fromv4f(v4f v) { u128 x; __builtin_memcpy(&x, &v, 16); return x; }
static inline v2d asv2d(u128 x) { v2d v; __builtin_memcpy(&v, &x, 16); return v; }
static inline u128 fromv2d(v2d v) { u128 x; __builtin_memcpy(&x, &v, 16); return x; }
static inline u128 U_divps(u128 a, u128 b) { return fromv4f(asv4f(a) / asv4f(b)); }
static inline u128 U_divpd(u128 a, u128 b) { return fromv2d(asv2d(a) / asv2d(b)); }
static inline u128 U_minps(u128 a, u128 b) { v4f x = asv4f(a), y = asv4f(b), r; for (int i = 0; i < 4; i++) r[i] = x[i] < y[i] ? x[i] : y[i]; return fromv4f(r); }
static inline u128 U_maxps(u128 a, u128 b) { v4f x = asv4f(a), y = asv4f(b), r; for (int i = 0; i < 4; i++) r[i] = x[i] > y[i] ? x[i] : y[i]; return fromv4f(r); }
static inline u128 U_packssdw(u128 a, u128 b) {
    s32 s[8]; __builtin_memcpy(s, &a, 16); __builtin_memcpy(s + 4, &b, 16); s16 o[8];
    for (int i = 0; i < 8; i++) o[i] = s[i] > 32767 ? 32767 : s[i] < -32768 ? -32768 : (s16)s[i];
    u128 r; __builtin_memcpy(&r, o, 16); return r;
}
static inline u128 U_psubusb(u128 a, u128 b) { u8 x[16], y[16]; __builtin_memcpy(x, &a, 16); __builtin_memcpy(y, &b, 16); for (int i = 0; i < 16; i++) x[i] = x[i] > y[i] ? x[i] - y[i] : 0; u128 r; __builtin_memcpy(&r, x, 16); return r; }
static inline u128 U_psllw(u128 a, u128 c) { u64 n = (u64)c; u16 x[8]; __builtin_memcpy(x, &a, 16); for (int i = 0; i < 8; i++) x[i] = n > 15 ? 0 : (u16)(x[i] << n); u128 r; __builtin_memcpy(&r, x, 16); return r; }
static inline u128 U_pshuflw(u128 dst, u128 src, u64 imm) { (void)dst; u16 s[8], o[8]; __builtin_memcpy(s, &src, 16); __builtin_memcpy(o, &src, 16); for (int i = 0; i < 4; i++) o[i] = s[(imm >> (2 * i)) & 3]; u128 r; __builtin_memcpy(&r, o, 16); return r; }
static inline u128 U_pshufhw(u128 dst, u128 src, u64 imm) { (void)dst; u16 s[8], o[8]; __builtin_memcpy(s, &src, 16); __builtin_memcpy(o, &src, 16); for (int i = 0; i < 4; i++) o[4 + i] = s[4 + ((imm >> (2 * i)) & 3)]; u128 r; __builtin_memcpy(&r, o, 16); return r; }

/* hooks provided by the host */
#include <setjmp.h>
extern _Thread_local void *rt_cur_jb;
struct lift_ent { u64 addr; void (*fn)(uint8_t *restrict); };
extern const struct lift_ent lift_table[]; extern const unsigned lift_table_n;
void guest_call(uint8_t *R, u64 addr);     /* indirect call: lifted function, host import or override */
void guest_jump(uint8_t *R, u64 addr);     /* indirect/tail jump, ends the current function */
u64 host_rdtsc(void);
void host_lock(void); void host_unlock(void);
void lift_fatal(const char *what);
#define HOST_LOCK() host_lock()
#define HOST_UNLOCK() host_unlock()
#define DIVTRAP() lift_fatal("integer divide by zero")
#define UD2TRAP() lift_fatal("invalid instruction")
#define UNSUPPORTED(op) lift_fatal("unsupported p-code op " op)
#define UNSUPPORTED_USEROP(op) lift_fatal("unsupported p-code userop " op)
