/*******************************************************************************
 * pfdaq_pmp_hw.h - demo variant only (PFDAQ_AMP_M09), project code.
 * SPDX-License-Identifier: MIT
 *
 * The hardware half of the PMP experiment: reading and writing the PMP CSRs, and the one excursion into
 * user mode. Include it only in a RISC-V build; the logic is in pfdaq_pmp.h.
 *
 * The excursion, step by step (m09_user_run):
 *   1. save the callee-saved registers and the four CSRs that are changed (mtvec, mscratch, mstatus, mie)
 *      on the stack of the calling task;
 *   2. clear mie: no interrupt can arrive while the hart is in user mode (an interrupt in user mode is
 *      always taken, whatever mstatus.MIE says, and would run FreeRTOS' handler on the user stack);
 *   3. point mtvec at m09_trap, keep the task's stack pointer in mscratch, set mepc to the user code,
 *      clear mstatus.MPP (user) and mstatus.MPRV, and mret;
 *   4. m09_trap logs every load or store access fault (mcause 5 or 7), skips the faulting instruction and
 *      returns to user mode; the ecall that ends the test, a full log, or any other trap ends the
 *      excursion: it restores the four CSRs and the registers and returns to the caller as a normal return.
 * Nothing here ever writes the lock bit; FreeRTOS' own trap vector, mret and registers are put back
 * exactly as they were, and the scheduler never runs in between (interrupts are masked throughout).
 ******************************************************************************/
#ifndef PFDAQ_PMP_HW_H
#define PFDAQ_PMP_HW_H

#include <stdint.h>
#include <string.h>
#include "pfdaq_pmp.h"

/* ---- CSR access ----------------------------------------------------------------------------------- */
#define M09_CSR_READ(csr, out)  __asm__ volatile ("csrr %0, " #csr : "=r"(out))
#define M09_CSR_WRITE(csr, val) __asm__ volatile ("csrw " #csr ", %0" :: "r"(val) : "memory")

static inline uint64_t m09_rd_cfg0(void) { uint64_t v; M09_CSR_READ(pmpcfg0, v); return v; }
static inline uint64_t m09_rd_cfg2(void) { uint64_t v; M09_CSR_READ(pmpcfg2, v); return v; }
static inline void m09_wr_cfg0(uint64_t v) { M09_CSR_WRITE(pmpcfg0, v); }
static inline void m09_wr_cfg2(uint64_t v) { M09_CSR_WRITE(pmpcfg2, v); }

#define M09_ADDR_CASES(X) \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)

static inline uint64_t m09_rd_addr(unsigned i)
{
    uint64_t v = 0;
    switch (i) {
#define X(n) case n: M09_CSR_READ(pmpaddr##n, v); break;
    M09_ADDR_CASES(X)
#undef X
    default: break;
    }
    return v;
}

static inline void m09_wr_addr(unsigned i, uint64_t v)
{
    switch (i) {
#define X(n) case n: M09_CSR_WRITE(pmpaddr##n, v); break;
    M09_ADDR_CASES(X)
#undef X
    default: break;
    }
}

static void m09_hw_read_state(struct pmp_state *s)
{
    s->cfg0 = m09_rd_cfg0();
    s->cfg2 = m09_rd_cfg2();
    for (unsigned i = 0; i < PMP_ENTRIES; i++)
        s->addr[i] = m09_rd_addr(i);
}

/*
 * Make the PMP equal to `want`, touching only the entries that differ from what is there now, and never
 * an entry that is locked (the hardware would ignore the write anyway) and never writing a lock bit.
 * Order: switch the entries to be changed off, write their addresses, then write their configuration.
 */
static void m09_hw_write_state(const struct pmp_state *want)
{
    struct pmp_state cur, tmp;
    unsigned changed[PMP_ENTRIES], n = 0;

    m09_hw_read_state(&cur);
    for (unsigned i = 0; i < PMP_ENTRIES; i++) {
        uint8_t c = pmp_cfg(&cur, i), w = pmp_cfg(want, i);
        if (c & PMP_L)
            continue;                                   /* locked: cannot change, do not try */
        if (w & PMP_L)
            continue;                                   /* this demo never writes a lock bit */
        if (c != w || (cur.addr[i] & M09_PMPADDR_MASK) != (want->addr[i] & M09_PMPADDR_MASK))
            changed[n++] = i;
    }
    if (!n)
        return;
    tmp = cur;
    for (unsigned k = 0; k < n; k++)
        pmp_set_cfg(&tmp, changed[k], 0);               /* off */
    m09_wr_cfg0(tmp.cfg0);
    m09_wr_cfg2(tmp.cfg2);
    for (unsigned k = 0; k < n; k++)
        m09_wr_addr(changed[k], want->addr[changed[k]]);
    for (unsigned k = 0; k < n; k++)
        pmp_set_cfg(&tmp, changed[k], pmp_cfg(want, changed[k]));
    m09_wr_cfg0(tmp.cfg0);
    m09_wr_cfg2(tmp.cfg2);
    __asm__ volatile ("sfence.vma x0, x0" ::: "memory");  /* the privileged specification asks for it after a PMP change */
}

/* ---- the user-mode excursion --------------------------------------------------------------------- */
/* layout of struct m09_ctx, used by the assembler below (checked by _Static_assert) */
#define M09_CTX_USER_SP     0
#define M09_CTX_OWN         8
#define M09_CTX_RING        16
#define M09_CTX_CANARY      24
#define M09_CTX_LINUX       32
#define M09_CTX_HSS         40
#define M09_CTX_NLOG        48
#define M09_CTX_ENDED       56
#define M09_CTX_ABORT_CAUSE 64
#define M09_CTX_ABORT_TVAL  72
#define M09_CTX_ABORT_EPC   80
#define M09_CTX_PROGRESS    88
#define M09_CTX_LOG0        96          /* log[0].cause; log[0].tval at +8, .epc at +16; 24 bytes per entry */
#define M09_CTX_LOG1        104
#define M09_CTX_LOG2        112

struct m09_ctx {
    uint64_t user_sp, own, ring, canary, linux_addr, hss_addr, nlog, ended;
    uint64_t abort_cause, abort_tval, abort_epc, progress;
    struct m09_fault log[M09_LOG_MAX];
};

_Static_assert(__builtin_offsetof(struct m09_ctx, user_sp) == M09_CTX_USER_SP, "ctx layout");
_Static_assert(__builtin_offsetof(struct m09_ctx, own) == M09_CTX_OWN, "ctx layout");
_Static_assert(__builtin_offsetof(struct m09_ctx, ring) == M09_CTX_RING, "ctx layout");
_Static_assert(__builtin_offsetof(struct m09_ctx, canary) == M09_CTX_CANARY, "ctx layout");
_Static_assert(__builtin_offsetof(struct m09_ctx, linux_addr) == M09_CTX_LINUX, "ctx layout");
_Static_assert(__builtin_offsetof(struct m09_ctx, hss_addr) == M09_CTX_HSS, "ctx layout");
_Static_assert(__builtin_offsetof(struct m09_ctx, nlog) == M09_CTX_NLOG, "ctx layout");
_Static_assert(__builtin_offsetof(struct m09_ctx, ended) == M09_CTX_ENDED, "ctx layout");
_Static_assert(__builtin_offsetof(struct m09_ctx, abort_cause) == M09_CTX_ABORT_CAUSE, "ctx layout");
_Static_assert(__builtin_offsetof(struct m09_ctx, abort_tval) == M09_CTX_ABORT_TVAL, "ctx layout");
_Static_assert(__builtin_offsetof(struct m09_ctx, abort_epc) == M09_CTX_ABORT_EPC, "ctx layout");
_Static_assert(__builtin_offsetof(struct m09_ctx, progress) == M09_CTX_PROGRESS, "ctx layout");
_Static_assert(__builtin_offsetof(struct m09_ctx, log) == M09_CTX_LOG0, "ctx layout");
_Static_assert(sizeof(struct m09_fault) == 24 && M09_LOG_MAX == 8, "the handler indexes the log with 24 bytes per entry and 8 entries");

#define M09_S2(x) #x
#define M09_S(x)  M09_S2(x)

/* frame of m09_user_run: ra, s0..s11 at 0..96, then the saved CSRs */
#define M09_FR_MTVEC     104
#define M09_FR_MSCRATCH  112
#define M09_FR_MSTATUS   120
#define M09_FR_MIE       128
#define M09_FR_SIZE      144

__asm__(
    "    .option push\n"
    "    .option norvc\n"
    "    .text\n"
    "    .balign 4\n"
    "    .globl m09_user_run\n"
    "    .type  m09_user_run, @function\n"
    "m09_user_run:\n"
    "    addi  sp, sp, -" M09_S(M09_FR_SIZE) "\n"
    "    sd    ra,  0(sp)\n"
    "    sd    s0,  8(sp)\n"
    "    sd    s1, 16(sp)\n"
    "    sd    s2, 24(sp)\n"
    "    sd    s3, 32(sp)\n"
    "    sd    s4, 40(sp)\n"
    "    sd    s5, 48(sp)\n"
    "    sd    s6, 56(sp)\n"
    "    sd    s7, 64(sp)\n"
    "    sd    s8, 72(sp)\n"
    "    sd    s9, 80(sp)\n"
    "    sd    s10, 88(sp)\n"
    "    sd    s11, 96(sp)\n"
    "    csrr  t0, mtvec\n"
    "    sd    t0, " M09_S(M09_FR_MTVEC) "(sp)\n"
    "    csrr  t0, mscratch\n"
    "    sd    t0, " M09_S(M09_FR_MSCRATCH) "(sp)\n"
    "    csrr  t0, mstatus\n"
    "    sd    t0, " M09_S(M09_FR_MSTATUS) "(sp)\n"
    "    csrr  t0, mie\n"
    "    sd    t0, " M09_S(M09_FR_MIE) "(sp)\n"
    "    csrw  mie, zero\n"                       /* no interrupt while in user mode */
    "    la    t0, m09_trap\n"
    "    csrw  mtvec, t0\n"                       /* direct mode: m09_trap is 4-byte aligned */
    "    csrw  mscratch, sp\n"                    /* the handler finds the task's stack here */
    "    la    t0, m09_user_entry\n"
    "    csrw  mepc, t0\n"
    "    li    t0, 0x00021800\n"                  /* MPP (bits 12:11) and MPRV (bit 17) */
    "    csrc  mstatus, t0\n"                     /* MPP = user, MPRV = 0 */
    "    la    a0, m09_ctx\n"
    "    ld    sp, " M09_S(M09_CTX_USER_SP) "(a0)\n"
    "    mret\n"
    "    .size m09_user_run, .-m09_user_run\n"
    "\n"
    /* ---- the user-mode code: six loads and stores of the test, then ecall ---- */
    "    .balign 4\n"
    "    .globl m09_user_entry\n"
    "    .type  m09_user_entry, @function\n"
    "m09_user_entry:\n"
    "    ld    t0, " M09_S(M09_CTX_OWN) "(a0)\n"
    "    ld    t1, 0(t0)\n"                       /* allowed: load of the firmware's own data word */
    "    addi  t1, t1, 1\n"
    "    sd    t1, 0(t0)\n"                       /* allowed: store to the same word */
    "    ld    t2, " M09_S(M09_CTX_RING) "(a0)\n"
    "    ld    t3, 0(t2)\n"                       /* allowed: load from the shared rings (read only: never written here) */
    "    ld    t4, " M09_S(M09_CTX_CANARY) "(a0)\n"
    "    ld    t5, 0(t4)\n"                       /* allowed: load from the read-only canary page */
    "    li    t6, 1\n"
    "    sd    t6, " M09_S(M09_CTX_PROGRESS) "(a0)\n"
    "    ld    t2, " M09_S(M09_CTX_LINUX) "(a0)\n"
    "    ld    t3, 0(t2)\n"                       /* violation 1: load from Linux DDR */
    "    ld    t2, " M09_S(M09_CTX_HSS) "(a0)\n"
    "    ld    t3, 0(t2)\n"                       /* violation 2: load from the boot monitor's L2 scratchpad */
    "    sd    t1, 0(t4)\n"                       /* violation 3: store into the read-only canary */
    "    li    t6, 2\n"
    "    sd    t6, " M09_S(M09_CTX_PROGRESS) "(a0)\n"
    "    ecall\n"                                 /* the test is over */
    "1:  j     1b\n"                              /* never reached: ecall always traps to m09_trap */
    "    .size m09_user_entry, .-m09_user_entry\n"
    "\n"
    /* ---- the trap handler ---- */
    "    .balign 4\n"
    "    .globl m09_trap\n"
    "    .type  m09_trap, @function\n"
    "m09_trap:\n"
    "    csrrw sp, mscratch, sp\n"                /* sp = frame of m09_user_run, mscratch = user sp */
    "    addi  sp, sp, -48\n"
    "    sd    t0,  0(sp)\n"
    "    sd    t1,  8(sp)\n"
    "    sd    t2, 16(sp)\n"
    "    sd    t3, 24(sp)\n"
    "    la    t0, m09_ctx\n"
    "    csrr  t1, mcause\n"
    "    li    t2, 8\n"
    "    beq   t1, t2, .Lm09_ecall\n"             /* ecall from user mode */
    "    li    t2, 5\n"
    "    beq   t1, t2, .Lm09_fault\n"             /* load access fault */
    "    li    t2, 7\n"
    "    bne   t1, t2, .Lm09_abort\n"             /* not a store access fault either: stop */
    ".Lm09_fault:\n"
    "    ld    t2, " M09_S(M09_CTX_NLOG) "(t0)\n"
    "    li    t3, 8\n"
    "    bgeu  t2, t3, .Lm09_abort\n"             /* log full: stop, so a loop of faults cannot run on */
    "    slli  t3, t2, 3\n"
    "    slli  t1, t2, 4\n"
    "    add   t1, t1, t3\n"                      /* 24 bytes per log entry */
    "    add   t1, t1, t0\n"
    "    csrr  t3, mcause\n"
    "    sd    t3, " M09_S(M09_CTX_LOG0) "(t1)\n"
    "    csrr  t3, mtval\n"
    "    sd    t3, " M09_S(M09_CTX_LOG1) "(t1)\n"
    "    csrr  t3, mepc\n"
    "    sd    t3, " M09_S(M09_CTX_LOG2) "(t1)\n"
    "    addi  t2, t2, 1\n"
    "    sd    t2, " M09_S(M09_CTX_NLOG) "(t0)\n"
    "    lhu   t1, 0(t3)\n"                       /* t3 = mepc: how long is the faulting instruction? */
    "    andi  t1, t1, 3\n"
    "    li    t2, 3\n"
    "    addi  t3, t3, 2\n"
    "    bne   t1, t2, 1f\n"
    "    addi  t3, t3, 2\n"
    "1:  csrw  mepc, t3\n"                        /* continue after it, still in user mode (MPP is user) */
    "    ld    t0,  0(sp)\n"
    "    ld    t1,  8(sp)\n"
    "    ld    t2, 16(sp)\n"
    "    ld    t3, 24(sp)\n"
    "    addi  sp, sp, 48\n"
    "    csrrw sp, mscratch, sp\n"                /* sp = user sp again, mscratch = frame */
    "    mret\n"
    ".Lm09_ecall:\n"
    "    li    t1, 1\n"
    "    sd    t1, " M09_S(M09_CTX_ENDED) "(t0)\n"
    "    j     .Lm09_leave\n"
    ".Lm09_abort:\n"
    "    li    t1, 2\n"
    "    sd    t1, " M09_S(M09_CTX_ENDED) "(t0)\n"
    "    csrr  t1, mcause\n"
    "    sd    t1, " M09_S(M09_CTX_ABORT_CAUSE) "(t0)\n"
    "    csrr  t1, mtval\n"
    "    sd    t1, " M09_S(M09_CTX_ABORT_TVAL) "(t0)\n"
    "    csrr  t1, mepc\n"
    "    sd    t1, " M09_S(M09_CTX_ABORT_EPC) "(t0)\n"
    ".Lm09_leave:\n"
    "    addi  sp, sp, 48\n"                      /* sp = frame of m09_user_run */
    "    ld    t0, " M09_S(M09_FR_MTVEC) "(sp)\n"
    "    csrw  mtvec, t0\n"
    "    ld    t0, " M09_S(M09_FR_MSCRATCH) "(sp)\n"
    "    csrw  mscratch, t0\n"
    "    ld    t0, " M09_S(M09_FR_MSTATUS) "(sp)\n"
    "    csrw  mstatus, t0\n"
    "    ld    ra,  0(sp)\n"
    "    ld    s0,  8(sp)\n"
    "    ld    s1, 16(sp)\n"
    "    ld    s2, 24(sp)\n"
    "    ld    s3, 32(sp)\n"
    "    ld    s4, 40(sp)\n"
    "    ld    s5, 48(sp)\n"
    "    ld    s6, 56(sp)\n"
    "    ld    s7, 64(sp)\n"
    "    ld    s8, 72(sp)\n"
    "    ld    s9, 80(sp)\n"
    "    ld    s10, 88(sp)\n"
    "    ld    s11, 96(sp)\n"
    "    ld    t0, " M09_S(M09_FR_MIE) "(sp)\n"
    "    addi  sp, sp, " M09_S(M09_FR_SIZE) "\n"
    "    li    a0, 0\n"
    "    csrw  mie, t0\n"                         /* last: an interrupt that became pending is taken now, in the restored context */
    "    ret\n"
    "    .size m09_trap, .-m09_trap\n"
    "    .option pop\n"
);

extern uint64_t m09_user_run(void);

/* everything the user code touches lives in the firmware image, inside the 1 MiB entry of the plan */
#define M09_STACK_WORDS 128
volatile struct m09_ctx m09_ctx;
static uint64_t m09_user_stack[M09_STACK_WORDS] __attribute__((aligned(16)));
static volatile uint64_t m09_own_word;
/* a page of its own, 4 KiB aligned: the plan makes it read-only for user mode, so a denied store aims at it */
static volatile uint64_t m09_canary_page[512] __attribute__((aligned(4096)));

#define M09_CANARY_VALUE 0xCA7A57A1CA7A57A1ull

static uint64_t m09_hw_canary_base(void) { return (uint64_t)(uintptr_t)m09_canary_page; }
static uint64_t m09_hw_own_addr(void) { return (uint64_t)(uintptr_t)&m09_own_word; }

static int m09_hw_run_user(struct m09_run *r)
{
    volatile struct m09_ctx *c = &m09_ctx;

    memset((void *)c, 0, sizeof(*c));
    c->user_sp = (uint64_t)(uintptr_t)&m09_user_stack[M09_STACK_WORDS];
    c->own = r->probe_addr[M09_P_OWN_DATA];
    c->ring = r->probe_addr[M09_P_RING_READ];
    c->canary = r->probe_addr[M09_P_CANARY_STORE];
    c->linux_addr = r->probe_addr[M09_P_LINUX_LOAD];
    c->hss_addr = r->probe_addr[M09_P_HSS_LOAD];
    m09_own_word = 1000;
    m09_canary_page[0] = M09_CANARY_VALUE;
    r->own_before = m09_own_word;
    r->canary_before = m09_canary_page[0];
    __asm__ volatile ("fence rw, rw" ::: "memory");

    (void)m09_user_run();

    __asm__ volatile ("fence rw, rw" ::: "memory");
    r->own_after = m09_own_word;
    r->canary_after = m09_canary_page[0];
    r->nlog = (unsigned)c->nlog;
    if (r->nlog > M09_LOG_MAX)
        r->nlog = M09_LOG_MAX;
    for (unsigned i = 0; i < r->nlog; i++) {
        r->log[i].cause = c->log[i].cause;
        r->log[i].tval = c->log[i].tval;
        r->log[i].epc = c->log[i].epc;
    }
    r->ended = (int)c->ended;
    r->abort_cause = c->abort_cause;
    r->abort_tval = c->abort_tval;
    r->abort_epc = c->abort_epc;
    return 0;
}

#endif /* PFDAQ_PMP_HW_H */
