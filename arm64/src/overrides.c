/* Host versions of the game's x87 libm routines (the translated x87 code would be slow and only double-precision anyway).
 * Same set the Rosetta build replaces, so both ports compute the same results. Each one is a translated-function
 * stand-in: arguments in xmm0/xmm1, result in xmm0, then the "ret". */
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#define XMM(n) (0x1200 + 0x40 * (n))
static inline float  gf(uint8_t *R, int n) { float v; memcpy(&v, R + XMM(n), 4); return v; }
static inline double gd(uint8_t *R, int n) { double v; memcpy(&v, R + XMM(n), 8); return v; }
static inline void ret(uint8_t *R) { uint64_t sp; memcpy(&sp, R + 0x20, 8); sp += 8; memcpy(R + 0x20, &sp, 8); }
static inline void rf(uint8_t *R, float v) { memset(R + XMM(0), 0, 16); memcpy(R + XMM(0), &v, 4); ret(R); }
static inline void rd(uint8_t *R, double v) { memset(R + XMM(0), 0, 16); memcpy(R + XMM(0), &v, 8); ret(R); }
#define F1(addr, expr) void f_##addr(uint8_t *restrict R) { rf(R, expr); }
#define D1(addr, expr) void f_##addr(uint8_t *restrict R) { rd(R, expr); }
F1(140339b30, (float)log2((double)gf(R, 0)))
D1(140339ac0, log2(gd(R, 0)))
F1(140339bd0, nearbyintf(gf(R, 0)))
D1(140339b90, nearbyint(gd(R, 0)))
F1(140339000, acosf(gf(R, 0)))
F1(140339030, asinf(gf(R, 0)))
F1(140339060, atan2f(gf(R, 0), gf(R, 1)))
D1(140339090, atan2(gd(R, 0), gd(R, 1)))
F1(140339880, fmodf(gf(R, 0), gf(R, 1)))
D1(140339850, fmod(gd(R, 0), gd(R, 1)))

/* scud_public_env_lock deletes every M3_* variable at start-up (so online play cannot be tuned). SCUD_HOST_KEEP_ENV=1 skips
 * it for offline use, which lets the game's own tuning variables (render threads etc.) through. Never use it online. */
#include <stdlib.h>
void f_140172ee0_lifted(uint8_t *restrict R);
void f_140172ee0(uint8_t *restrict R) {
    static int keep = -1;
    if (keep < 0) { keep = getenv("SCUD_HOST_KEEP_ENV") != NULL; if (keep) fprintf(stderr, "[host] SCUD_HOST_KEEP_ENV: the game's M3_* environment lock is skipped (offline only)\n"); }
    if (keep) ret(R); else f_140172ee0_lifted(R);
}
