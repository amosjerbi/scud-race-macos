/* Fiber context switch and Win64 setjmp/longjmp for the macOS host. x86-64 SysV entry points
   (callers are clang ms_abi wrappers that already preserve rsi/rdi/xmm6-15 around SysV calls). */
    .text
    .globl fiber_swap
    .p2align 4
/* void fiber_swap(void **save_sp, void *new_sp) */
fiber_swap:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

    .globl fiber_boot
    .p2align 4
/* first thing a new fiber runs; r12 = struct fiber* (seeded in the initial frame) */
fiber_boot:
    movq %r12, %rdi
    call fiber_main
    ud2

/* Win64 jmp_buf (msvcrt _JUMP_BUFFER is 256 bytes). We use:
   0 rbx 8 rbp 16 rdi 24 rsi 32 r12 40 r13 48 r14 56 r15 64 rsp 72 rip 80 mxcsr 96.. xmm6-15 (16 each) */
    .globl ms_setjmp
    .p2align 4
/* int ms_setjmp(jmp_buf *buf [rcx], void *frame [rdx]) -- Win64 convention */
ms_setjmp:
    movq %rbx, 0(%rcx)
    movq %rbp, 8(%rcx)
    movq %rdi, 16(%rcx)
    movq %rsi, 24(%rcx)
    movq %r12, 32(%rcx)
    movq %r13, 40(%rcx)
    movq %r14, 48(%rcx)
    movq %r15, 56(%rcx)
    leaq 8(%rsp), %rax
    movq %rax, 64(%rcx)
    movq (%rsp), %rax
    movq %rax, 72(%rcx)
    stmxcsr 80(%rcx)
    movups %xmm6, 96(%rcx)
    movups %xmm7, 112(%rcx)
    movups %xmm8, 128(%rcx)
    movups %xmm9, 144(%rcx)
    movups %xmm10, 160(%rcx)
    movups %xmm11, 176(%rcx)
    movups %xmm12, 192(%rcx)
    movups %xmm13, 208(%rcx)
    movups %xmm14, 224(%rcx)
    movups %xmm15, 240(%rcx)
    xorl %eax, %eax
    ret

    .globl ms_longjmp
    .p2align 4
/* void ms_longjmp(jmp_buf *buf [rcx], int val [edx]) */
ms_longjmp:
    movl %edx, %eax
    testl %eax, %eax
    jne 1f
    movl $1, %eax
1:
    movq 0(%rcx), %rbx
    movq 8(%rcx), %rbp
    movq 16(%rcx), %rdi
    movq 24(%rcx), %rsi
    movq 32(%rcx), %r12
    movq 40(%rcx), %r13
    movq 48(%rcx), %r14
    movq 56(%rcx), %r15
    ldmxcsr 80(%rcx)
    movups 96(%rcx), %xmm6
    movups 112(%rcx), %xmm7
    movups 128(%rcx), %xmm8
    movups 144(%rcx), %xmm9
    movups 160(%rcx), %xmm10
    movups 176(%rcx), %xmm11
    movups 192(%rcx), %xmm12
    movups 208(%rcx), %xmm13
    movups 224(%rcx), %xmm14
    movups 240(%rcx), %xmm15
    movq 64(%rcx), %rsp
    jmpq *72(%rcx)

    .globl tracer_common
    .p2align 4
/* Transparent call tracer: entered via `mov r11, name; mov r10, target; jmp tracer_common`, Win64 args intact. */
tracer_common:
    pushq %rcx
    pushq %rdx
    pushq %r8
    pushq %r9
    pushq %rsi
    pushq %rdi
    pushq %r10
    subq $0x100, %rsp
    movups %xmm0, 0(%rsp)
    movups %xmm1, 16(%rsp)
    movups %xmm2, 32(%rsp)
    movups %xmm3, 48(%rsp)
    movups %xmm4, 64(%rsp)
    movups %xmm5, 80(%rsp)
    movups %xmm6, 96(%rsp)
    movups %xmm7, 112(%rsp)
    movups %xmm8, 128(%rsp)
    movups %xmm9, 144(%rsp)
    movups %xmm10, 160(%rsp)
    movups %xmm11, 176(%rsp)
    movups %xmm12, 192(%rsp)
    movups %xmm13, 208(%rsp)
    movups %xmm14, 224(%rsp)
    movups %xmm15, 240(%rsp)
    movq %r11, %rdi
    call trace_call
    movups 0(%rsp), %xmm0
    movups 16(%rsp), %xmm1
    movups 32(%rsp), %xmm2
    movups 48(%rsp), %xmm3
    movups 64(%rsp), %xmm4
    movups 80(%rsp), %xmm5
    movups 96(%rsp), %xmm6
    movups 112(%rsp), %xmm7
    movups 128(%rsp), %xmm8
    movups 144(%rsp), %xmm9
    movups 160(%rsp), %xmm10
    movups 176(%rsp), %xmm11
    movups 192(%rsp), %xmm12
    movups 208(%rsp), %xmm13
    movups 224(%rsp), %xmm14
    movups 240(%rsp), %xmm15
    addq $0x100, %rsp
    popq %r10
    popq %rdi
    popq %rsi
    popq %r9
    popq %r8
    popq %rdx
    popq %rcx
    jmpq *%r10

    .globl gs20_stub
    .p2align 4
/* Replaces `mov rax, gs:[0x20]` (GetCurrentFiber): returns this thread's current fiber in rax, preserves every other register. */
gs20_stub:
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    subq $0x108, %rsp
    movups %xmm0, 0(%rsp)
    movups %xmm1, 16(%rsp)
    movups %xmm2, 32(%rsp)
    movups %xmm3, 48(%rsp)
    movups %xmm4, 64(%rsp)
    movups %xmm5, 80(%rsp)
    movups %xmm6, 96(%rsp)
    movups %xmm7, 112(%rsp)
    movups %xmm8, 128(%rsp)
    movups %xmm9, 144(%rsp)
    movups %xmm10, 160(%rsp)
    movups %xmm11, 176(%rsp)
    movups %xmm12, 192(%rsp)
    movups %xmm13, 208(%rsp)
    movups %xmm14, 224(%rsp)
    movups %xmm15, 240(%rsp)
    call host_current_fiber
    movups 0(%rsp), %xmm0
    movups 16(%rsp), %xmm1
    movups 32(%rsp), %xmm2
    movups 48(%rsp), %xmm3
    movups 64(%rsp), %xmm4
    movups 80(%rsp), %xmm5
    movups 96(%rsp), %xmm6
    movups 112(%rsp), %xmm7
    movups 128(%rsp), %xmm8
    movups 144(%rsp), %xmm9
    movups 160(%rsp), %xmm10
    movups 176(%rsp), %xmm11
    movups 192(%rsp), %xmm12
    movups 208(%rsp), %xmm13
    movups 224(%rsp), %xmm14
    movups 240(%rsp), %xmm15
    addq $0x108, %rsp
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    ret

    .section .note.GNU-stack,"",@progbits
