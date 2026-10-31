/* Win32 fibers for the translated game (its m3_coro_* coroutines).
 * A fiber owns a native C stack (the translated code recurses on it) and a guest stack (the x86-64 rsp); a switch swaps
 * the native context with ucontext and the guest's callee-saved registers through the per-thread register file. */
#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif
#include <ucontext.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

typedef uint64_t u64;
typedef uint32_t DWORD;
typedef int32_t BOOL;
uint8_t *rt_regs(void);
uint64_t *rt_teb(void);
void rt_save_callee(uint8_t *R, uint8_t *save);
void rt_restore_callee(uint8_t *R, const uint8_t *save);
void rt_set_rsp(u64 v);
uint64_t call_guest(uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
void host_end_thread(DWORD ret);
void log_msg(const char *fmt, ...);

/* GetFiberData() reads the first qword of the fiber (gs:[0x20] points here), so param stays first */
struct fiber {
    void *param; u64 fn; ucontext_t uc;
    uint8_t *cstack; size_t csize; uint8_t *gstack; size_t gsize;
    uint8_t save[256];
};
static _Thread_local struct fiber *t_cur;

static void set_cur(struct fiber *f) { t_cur = f; rt_teb()[4] = (u64)(uintptr_t)f; }

void *rt_ConvertThreadToFiberEx(void *param, DWORD flags) {
    (void)flags; struct fiber *f = calloc(1, sizeof *f); f->param = param; set_cur(f); return f;
}
BOOL rt_IsThreadAFiber(void) { return t_cur != NULL; }

static void fiber_entry(unsigned lo, unsigned hi) {
    struct fiber *f = (struct fiber *)(uintptr_t)(((u64)hi << 32) | lo);
    rt_set_rsp(((u64)(uintptr_t)(f->gstack + f->gsize) - 0x100) & ~0xFull);
    call_guest(f->fn, (u64)(uintptr_t)f->param, 0, 0, 0);
    log_msg("a fiber function returned (that ends the thread on Windows)");
    host_end_thread(0);
}

void *rt_CreateFiberEx(size_t commit, size_t reserve, DWORD flags, u64 fn, void *param) {
    (void)commit; (void)flags;
    struct fiber *f = calloc(1, sizeof *f); f->fn = fn; f->param = param;
    f->gsize = reserve > (1u << 20) ? (reserve + 0xFFFF) & ~(size_t)0xFFFF : (1u << 20);
    f->csize = 8u << 20;                                  /* translated frames are bigger than the original ones */
    f->gstack = mmap(NULL, f->gsize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    f->cstack = mmap(NULL, f->csize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (f->gstack == MAP_FAILED || f->cstack == MAP_FAILED) { free(f); return NULL; }
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp = f->cstack; f->uc.uc_stack.ss_size = f->csize; f->uc.uc_link = NULL;
    u64 p = (u64)(uintptr_t)f;
    makecontext(&f->uc, (void (*)(void))fiber_entry, 2, (unsigned)p, (unsigned)(p >> 32));
    return f;
}

void rt_SwitchToFiber(void *to) {
    struct fiber *t = to, *me = t_cur;
    if (!me || !t) { log_msg("SwitchToFiber(%p) on %s", to, me ? "a NULL target" : "a thread that is not a fiber"); abort(); }
    if (t == me) return;
    uint8_t *R = rt_regs();
    rt_save_callee(R, me->save);
    u64 rsp_ret; memcpy(&rsp_ret, me->save, 8);
    set_cur(t);
    swapcontext(&me->uc, &t->uc);
    /* back on this fiber */
    rt_restore_callee(R, me->save);
    set_cur(me);
}

void rt_DeleteFiber(void *fp) {
    struct fiber *f = fp; if (!f || f == t_cur) return;
    if (f->gstack) munmap(f->gstack, f->gsize);
    if (f->cstack) munmap(f->cstack, f->csize);
    free(f);
}
