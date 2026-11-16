/*******************************************************************************
 * pfdaq_pmp.h - demo variant only (PFDAQ_AMP_M09), project code.
 * SPDX-License-Identifier: MIT
 *
 * The logic of the PMP experiment, with no hardware access: decoding of the PMP configuration, the
 * access check as the RISC-V privileged specification defines it, the plan of the entries the demo
 * installs, and the decision whether installing them is possible at all. pfdaq_pmp_hw.h reads and
 * writes the real CSRs; the PC tests (experiments/amp-m09/tests) run this same file.
 *
 * RISC-V PMP in one paragraph (RV64). A hart has up to 16 entries. Entry i is described by a
 * configuration byte (R = bit 0, W = bit 1, X = bit 2, A = bits 3-4, L = bit 7) in pmpcfg0 (entries
 * 0-7) or pmpcfg2 (entries 8-15), and by pmpaddr<i>, which holds an address shifted right by two.
 * A = 0 off, 1 TOR (top of range, the bottom is pmpaddr<i-1>), 2 NA4 (four bytes), 3 NAPOT (a naturally
 * aligned power-of-two range, size in the number of trailing one bits of pmpaddr). For an access the
 * LOWEST-numbered entry that matches decides. Without the L bit an entry constrains only user and
 * supervisor mode, so machine mode ignores it; with L it constrains machine mode too and can no longer
 * be changed until the hart is reset. If no entry matches, machine mode succeeds and user mode fails
 * (when the hart implements any entry). This demo NEVER sets L.
 ******************************************************************************/
#ifndef PFDAQ_PMP_H
#define PFDAQ_PMP_H

#include <stdint.h>
#include <string.h>

#define PMP_ENTRIES   16

/* message types of the M09 demo variant (the production types are 1..6) */
#define M09_T_REPORT  0x40u       /* print the PMP report again; answered with the counts */
#define M09_T_RUN     0x41u       /* install the plan, run the user-mode test once, restore; answered with the result */

#define PMP_R         0x01u
#define PMP_W         0x02u
#define PMP_X         0x04u
#define PMP_A_MASK    0x18u
#define PMP_A_OFF     0x00u
#define PMP_A_TOR     0x08u
#define PMP_A_NA4     0x10u
#define PMP_A_NAPOT   0x18u
#define PMP_L         0x80u

enum pmp_priv { PMP_PRIV_U = 0, PMP_PRIV_S = 1, PMP_PRIV_M = 3 };
enum pmp_access { PMP_ACC_R = PMP_R, PMP_ACC_W = PMP_W, PMP_ACC_X = PMP_X };

/* the whole state: the two configuration registers and the 16 address registers */
struct pmp_state {
    uint64_t cfg0, cfg2;
    uint64_t addr[PMP_ENTRIES];
};

static inline uint8_t pmp_cfg(const struct pmp_state *s, unsigned i)
{
    return (uint8_t)((i < 8 ? s->cfg0 >> (8u * i) : s->cfg2 >> (8u * (i - 8u))) & 0xFFu);
}

static inline void pmp_set_cfg(struct pmp_state *s, unsigned i, uint8_t v)
{
    uint64_t *r = i < 8 ? &s->cfg0 : &s->cfg2;
    unsigned sh = 8u * (i < 8 ? i : i - 8u);
    *r = (*r & ~((uint64_t)0xFFu << sh)) | ((uint64_t)v << sh);
}

/* the range [lo, hi] (inclusive) an entry covers; returns 0 for an entry that is off */
static inline int pmp_range(const struct pmp_state *s, unsigned i, uint64_t *lo, uint64_t *hi)
{
    uint8_t c = pmp_cfg(s, i);
    uint64_t a = s->addr[i];

    switch (c & PMP_A_MASK) {
    case PMP_A_TOR:
        *lo = i ? s->addr[i - 1] << 2 : 0;
        *hi = (a << 2) - 1u;                     /* an empty range (top <= bottom) matches nothing, see pmp_check */
        if ((a << 2) <= *lo)
            return 0;
        return 1;
    case PMP_A_NA4:
        *lo = a << 2;
        *hi = *lo + 3u;
        return 1;
    case PMP_A_NAPOT: {
        unsigned k = 0;                          /* trailing one bits */
        while (k < 62 && ((a >> k) & 1u))
            k++;
        if (k >= 61) {                           /* the whole 56-bit physical space and beyond */
            *lo = 0;
            *hi = ~(uint64_t)0;
            return 1;
        }
        uint64_t size = (uint64_t)1 << (k + 3);
        *lo = (a & ~(((uint64_t)1 << (k + 1)) - 1u)) << 2;
        *hi = *lo + size - 1u;
        return 1;
    }
    default:
        return 0;
    }
}

/* number of entries that are not off, and of those with the lock bit */
static inline void pmp_count(const struct pmp_state *s, unsigned *active, unsigned *locked)
{
    *active = *locked = 0;
    for (unsigned i = 0; i < PMP_ENTRIES; i++) {
        uint8_t c = pmp_cfg(s, i);
        if ((c & PMP_A_MASK) != PMP_A_OFF)
            (*active)++;
        if ((c & PMP_A_MASK) != PMP_A_OFF && (c & PMP_L))
            (*locked)++;
    }
}

/*
 * Would an access of len bytes at addr, of the given kind, from the given privilege mode succeed?
 * The rules of the privileged specification: the lowest-numbered entry with any byte in range decides;
 * the access must lie wholly inside that entry; a machine-mode access passes an entry without L; otherwise
 * the permission bit of the kind decides. No match: machine mode passes, user and supervisor mode fail.
 * If `which` is not NULL it receives the deciding entry (-1 for none).
 */
static inline int pmp_check(const struct pmp_state *s, uint64_t addr, uint64_t len, enum pmp_access kind,
                            enum pmp_priv priv, int *which)
{
    uint64_t end = addr + len - 1u;                 /* len >= 1 */

    if (which)
        *which = -1;
    for (unsigned i = 0; i < PMP_ENTRIES; i++) {
        uint64_t lo, hi;
        uint8_t c = pmp_cfg(s, i);
        if (!pmp_range(s, i, &lo, &hi))
            continue;
        if (end < lo || addr > hi)
            continue;                               /* no byte of the access in this entry */
        if (which)
            *which = (int)i;
        if (addr < lo || end > hi)
            return 0;                               /* partial match: fails */
        if (priv == PMP_PRIV_M && !(c & PMP_L))
            return 1;
        return (c & kind) != 0;
    }
    return priv == PMP_PRIV_M;
}

/* ---- encoding ------------------------------------------------------------------------------------- */
/* pmpaddr value of a NAPOT range; base must be aligned to size, size a power of two >= 8 */
static inline uint64_t pmp_napot_addr(uint64_t base, uint64_t size)
{
    return (base | (size / 2u - 1u)) >> 2;
}

static inline int pmp_napot_ok(uint64_t base, uint64_t size)
{
    return size >= 8u && (size & (size - 1u)) == 0 && (base & (size - 1u)) == 0;
}

/* ---- the plan --------------------------------------------------------------------------------------- */
/*
 * Four entries, installed at index 0..3, user mode only (no L anywhere):
 *   0  the canary page: NAPOT 4 KiB, read only. Lower number than entry 1, so it wins inside its page.
 *   1  the firmware image: NAPOT 1 MiB at the hart 4 image base, read, write, execute (code, data, stack)
 *   2  off, only holds the bottom address of entry 3
 *   3  the shared rings and buffers: TOR from entry 2's address, read and write
 * Everything else is denied to user mode by the "no entry matches" rule.
 */
#define PMP_PLAN_ENTRIES   4

struct pmp_plan {
    uint8_t cfg[PMP_PLAN_ENTRIES];
    uint64_t addr[PMP_PLAN_ENTRIES];
};

/* returns 0 and fills the plan, or -1 if the geometry is not encodable */
static inline int pmp_plan_build(struct pmp_plan *p, uint64_t canary_base, uint64_t fw_base, uint64_t fw_size,
                                 uint64_t ring_lo, uint64_t ring_hi)
{
    if (!pmp_napot_ok(canary_base, 4096u) || !pmp_napot_ok(fw_base, fw_size) || ring_hi <= ring_lo ||
        (ring_lo & 3u) || (ring_hi & 3u))
        return -1;
    if (canary_base < fw_base || canary_base + 4096u > fw_base + fw_size)
        return -1;                                  /* the canary lives inside the image, so a stray store hurts nothing else */
    p->cfg[0] = (uint8_t)(PMP_A_NAPOT | PMP_R);
    p->addr[0] = pmp_napot_addr(canary_base, 4096u);
    p->cfg[1] = (uint8_t)(PMP_A_NAPOT | PMP_R | PMP_W | PMP_X);
    p->addr[1] = pmp_napot_addr(fw_base, fw_size);
    p->cfg[2] = (uint8_t)PMP_A_OFF;
    p->addr[2] = ring_lo >> 2;
    p->cfg[3] = (uint8_t)(PMP_A_TOR | PMP_R | PMP_W);
    p->addr[3] = ring_hi >> 2;
    return 0;
}

/* the lock bit must never appear in anything the demo writes */
static inline int pmp_plan_has_lock(const struct pmp_plan *p)
{
    for (unsigned i = 0; i < PMP_PLAN_ENTRIES; i++)
        if (p->cfg[i] & PMP_L)
            return 1;
    return 0;
}

/* the state after the plan is applied over `cur`: entries 0..3 replaced, the others as found */
static inline struct pmp_state pmp_apply(const struct pmp_state *cur, const struct pmp_plan *p)
{
    struct pmp_state s = *cur;
    for (unsigned i = 0; i < PMP_PLAN_ENTRIES; i++) {
        pmp_set_cfg(&s, i, p->cfg[i]);
        s.addr[i] = p->addr[i];
    }
    return s;
}

/* why the plan cannot be installed (0: it can) */
enum pmp_refusal { PMP_OK = 0, PMP_REFUSE_LOCKED = 1, PMP_REFUSE_LOCK_IN_PLAN = 2 };

/* locked-open: the first locked entry, if any, and whether it allows everything to everyone */
static inline int pmp_first_locked(const struct pmp_state *s, unsigned *index, int *open_to_all)
{
    for (unsigned i = 0; i < PMP_ENTRIES; i++) {
        uint8_t c = pmp_cfg(s, i);
        if ((c & PMP_A_MASK) != PMP_A_OFF && (c & PMP_L)) {
            uint64_t lo, hi;
            *index = i;
            /* the physical address space is 56 bits: a range from 0 up to at least 2^56 - 1 is "everything" */
            *open_to_all = pmp_range(s, i, &lo, &hi) && lo == 0 && hi >= 0x00FFFFFFFFFFFFFFull &&
                           (c & (PMP_R | PMP_W | PMP_X)) == (PMP_R | PMP_W | PMP_X);
            return 1;
        }
    }
    return 0;
}

/* a locked entry among indices 0..3 means the entries cannot be changed: the same checks the hardware makes */
static inline enum pmp_refusal pmp_can_install(const struct pmp_state *cur, const struct pmp_plan *p, unsigned *blocking)
{
    if (pmp_plan_has_lock(p))
        return PMP_REFUSE_LOCK_IN_PLAN;
    for (unsigned i = 0; i < PMP_PLAN_ENTRIES; i++) {
        /* a locked entry ignores writes to its cfg and to its pmpaddr; the pmpaddr of entry i is also frozen when
         * entry i+1 is a locked TOR entry, which matters for the bottom address of entry 3 (entry 2 here) */
        if (pmp_cfg(cur, i) & PMP_L) {
            *blocking = i;
            return PMP_REFUSE_LOCKED;
        }
    }
    if ((pmp_cfg(cur, PMP_PLAN_ENTRIES) & PMP_L) && (pmp_cfg(cur, PMP_PLAN_ENTRIES) & PMP_A_MASK) == PMP_A_TOR) {
        *blocking = PMP_PLAN_ENTRIES;               /* entry 4 is a locked TOR: pmpaddr3 is frozen */
        return PMP_REFUSE_LOCKED;
    }
    return PMP_OK;
}

/* ---- the probes of the test and what the model expects of them ------------------------------------------ */
enum m09_probe { M09_P_OWN_DATA, M09_P_RING_READ, M09_P_CANARY_READ, M09_P_LINUX_LOAD, M09_P_HSS_LOAD, M09_P_CANARY_STORE, M09_PROBES };

static inline int m09_probe_is_violation(int p) { return p >= M09_P_LINUX_LOAD; }

/* mcause the hardware raises for a denied access of each probe: 5 load access fault, 7 store/AMO access fault */
static inline unsigned m09_probe_cause(int p) { return p == M09_P_CANARY_STORE ? 7u : 5u; }

/* the number of faults the test must log if the PMP does its job */
#define M09_EXPECTED_FAULTS  3u

/* ---- the result of one run, as the hardware layer returns it ---------------------------------------------- */
#define M09_LOG_MAX  8u

struct m09_fault { uint64_t cause, tval, epc; };

enum m09_ended { M09_NOT_ENDED = 0, M09_ENDED_ECALL = 1, M09_ENDED_ABORT = 2 };

/* result codes sent back to Linux and printed */
enum m09_result {
    M09_RES_OK = 0,             /* the allowed accesses worked and exactly the three violations were caught */
    M09_RES_REFUSED = 1,        /* a locked entry blocks the plan: nothing installed, no user-mode test run */
    M09_RES_NOT_EFFECTIVE = 2,  /* the test ran, fewer than three faults: the fence does not hold (see the report) */
    M09_RES_ABORTED = 3,        /* the user-mode code was stopped by an unexpected trap (not an access fault of a probe) */
    M09_RES_INSTALL_FAILED = 4, /* the CSRs did not read back as written: restored, nothing run */
    M09_RES_BAD_GEOMETRY = 5
};

/* judge a run: faults[0..n) in order; the expected order is the three violations */
static inline enum m09_result m09_judge(unsigned n, const struct m09_fault *f, int ended, const uint64_t *probe_addr)
{
    if (ended != M09_ENDED_ECALL)
        return M09_RES_ABORTED;
    if (n != M09_EXPECTED_FAULTS)
        return M09_RES_NOT_EFFECTIVE;
    for (unsigned i = 0; i < n; i++) {
        int p = M09_P_LINUX_LOAD + (int)i;
        if (f[i].cause != m09_probe_cause(p) || f[i].tval != probe_addr[p])
            return M09_RES_NOT_EFFECTIVE;
    }
    return M09_RES_OK;
}


/* ===========================================================================================================
 * The sequence of the experiment, on top of five things the environment provides (struct m09_io): print a
 * line, read the PMP, write the PMP, run the user-mode test, and give the addresses. On the board
 * pfdaq_pmp_hw.h and pfdaq_supervisor.c provide them; the PC tests provide a model of the CSRs.
 * ========================================================================================================= */
struct m09_run {
    uint64_t probe_addr[M09_PROBES];       /* in: the address each probe uses */
    struct m09_fault log[M09_LOG_MAX];     /* out: access faults caught, in order */
    unsigned nlog;
    int ended;                             /* enum m09_ended */
    uint64_t abort_cause, abort_tval, abort_epc;
    uint64_t own_before, own_after;        /* the word the user code increments (allowed store) */
    uint64_t canary_before, canary_after;  /* the word the denied store aims at */
};

struct m09_geom {
    uint64_t canary, fw_base, fw_size, ring_lo, ring_hi;
    uint64_t own_data, linux_addr, hss_addr;
};

struct m09_io {
    void (*print)(const char *fmt, ...);
    void (*read_state)(struct pmp_state *s);
    /* write the state so that entries differing from the current one are changed in a safe order; a locked entry ignores the write */
    void (*write_state)(const struct pmp_state *s);
    /* run the small user-mode function once; returns 0 when it came back, non-zero if the hardware layer refused to run it */
    int (*run_user)(struct m09_run *r);
};

#define M09_PMPADDR_MASK  0x003FFFFFFFFFFFFFull   /* pmpaddr has 54 bits on RV64 */

static inline const char *m09_a_name(uint8_t c)
{
    switch (c & PMP_A_MASK) {
    case PMP_A_TOR: return "TOR";
    case PMP_A_NA4: return "NA4";
    case PMP_A_NAPOT: return "NAPOT";
    default: return "OFF";
    }
}

/* the report: what the boot monitor (and the hart's own start-up code) left. Reads only, safe at any time. */
static inline void m09_report(const struct m09_io *io, const struct pmp_state *s)
{
    unsigned active, locked, idx = 0;
    int open = 0;

    io->print("[m09] pmpcfg0 0x%016lx pmpcfg2 0x%016lx", (unsigned long)s->cfg0, (unsigned long)s->cfg2);
    for (unsigned i = 0; i < PMP_ENTRIES; i++) {
        uint8_t c = pmp_cfg(s, i);
        uint64_t lo = 0, hi = 0;
        if ((c & PMP_A_MASK) == PMP_A_OFF && !s->addr[i] && !c)
            continue;                                       /* empty entry: not worth a line */
        int on = pmp_range(s, i, &lo, &hi);
        io->print("[m09] pmp%-2u cfg 0x%02x %-5s %c%c%c%s pmpaddr 0x%014lx %s 0x%lx..0x%lx", i, c, m09_a_name(c),
                  (c & PMP_R) ? 'R' : '-', (c & PMP_W) ? 'W' : '-', (c & PMP_X) ? 'X' : '-', (c & PMP_L) ? " L" : "",
                  (unsigned long)s->addr[i], on ? "covers" : "(no range)", (unsigned long)lo, (unsigned long)hi);
    }
    pmp_count(s, &active, &locked);
    io->print("[m09] %u entries active, %u locked", active, locked);
    if (pmp_first_locked(s, &idx, &open))
        io->print("[m09] entry %u is locked%s: a locked entry cannot be changed before the hart is reset%s", idx,
                  open ? " and allows read, write and execute everywhere" : "",
                  open ? ", and being the lowest it decides every access: this hart's PMP is open to all modes" : "");
    else
        io->print("[m09] no entry is locked: entries can be changed, and none constrains machine mode");
}

static inline int m09_state_equal(const struct pmp_state *a, const struct pmp_state *b, unsigned n_addr)
{
    if (a->cfg0 != b->cfg0 || a->cfg2 != b->cfg2)
        return 0;
    for (unsigned i = 0; i < n_addr; i++)
        if ((a->addr[i] & M09_PMPADDR_MASK) != (b->addr[i] & M09_PMPADDR_MASK))
            return 0;
    return 1;
}

static inline const char *m09_result_name(enum m09_result r)
{
    switch (r) {
    case M09_RES_OK: return "OK";
    case M09_RES_REFUSED: return "REFUSED";
    case M09_RES_NOT_EFFECTIVE: return "NOT_EFFECTIVE";
    case M09_RES_ABORTED: return "ABORTED";
    case M09_RES_INSTALL_FAILED: return "INSTALL_FAILED";
    default: return "BAD_GEOMETRY";
    }
}

/*
 * The test. Install the plan (user mode only, no lock), run the user-mode function, put the entries back
 * as they were. Nothing is installed when a locked entry would ignore the write, and nothing is run when
 * the registers do not read back as written.
 */
static inline enum m09_result m09_run(const struct m09_io *io, const struct m09_geom *g, struct m09_run *run)
{
    struct pmp_state orig, now, want;
    struct pmp_plan plan;
    unsigned blocking = 0;
    enum m09_result res;

    memset(run, 0, sizeof(*run));
    run->probe_addr[M09_P_OWN_DATA] = g->own_data;
    run->probe_addr[M09_P_RING_READ] = g->ring_lo;
    run->probe_addr[M09_P_CANARY_READ] = g->canary;
    run->probe_addr[M09_P_LINUX_LOAD] = g->linux_addr;
    run->probe_addr[M09_P_HSS_LOAD] = g->hss_addr;
    run->probe_addr[M09_P_CANARY_STORE] = g->canary;

    io->read_state(&orig);
    if (pmp_plan_build(&plan, g->canary, g->fw_base, g->fw_size, g->ring_lo, g->ring_hi)) {
        io->print("[m09] the plan cannot be encoded for this memory layout: nothing installed");
        return M09_RES_BAD_GEOMETRY;
    }
    if (pmp_can_install(&orig, &plan, &blocking) != PMP_OK) {
        io->print("[m09] REFUSED: pmp%u is locked, so writes to it are ignored until the hart is reset. Nothing installed, "
                  "no user-mode test run, nothing changed", blocking);
        return M09_RES_REFUSED;
    }
    want = pmp_apply(&orig, &plan);

    io->print("[m09] plan: pmp0 canary page 0x%lx R, pmp1 image 0x%lx size 0x%lx RWX, pmp2 off (bottom 0x%lx), "
              "pmp3 rings up to 0x%lx RW; no L bit anywhere",
              (unsigned long)g->canary, (unsigned long)g->fw_base, (unsigned long)g->fw_size, (unsigned long)g->ring_lo,
              (unsigned long)g->ring_hi);
    for (int p = M09_P_LINUX_LOAD; p < M09_PROBES; p++) {
        int which;
        int allowed = pmp_check(&want, run->probe_addr[p], 8, p == M09_P_CANARY_STORE ? PMP_ACC_W : PMP_ACC_R, PMP_PRIV_U, &which);
        if (which < 0)
            io->print("[m09] model: user %s of 0x%lx is %s (no entry matches)", p == M09_P_CANARY_STORE ? "store" : "load",
                      (unsigned long)run->probe_addr[p], allowed ? "ALLOWED" : "denied");
        else
            io->print("[m09] model: user %s of 0x%lx is %s (decided by pmp%d)", p == M09_P_CANARY_STORE ? "store" : "load",
                      (unsigned long)run->probe_addr[p], allowed ? "ALLOWED" : "denied", which);
    }

    io->write_state(&want);
    io->read_state(&now);
    if (!m09_state_equal(&now, &want, PMP_PLAN_ENTRIES)) {
        io->write_state(&orig);
        io->print("[m09] the PMP did not read back as written (pmpcfg0 0x%lx, wanted 0x%lx): restored, no test run",
                  (unsigned long)now.cfg0, (unsigned long)want.cfg0);
        return M09_RES_INSTALL_FAILED;
    }
    io->print("[m09] installed and read back; entering user mode");

    if (io->run_user(run)) {
        io->write_state(&orig);
        io->print("[m09] the user-mode test was not run");
        return M09_RES_ABORTED;
    }

    io->write_state(&orig);
    io->read_state(&now);
    if (!m09_state_equal(&now, &orig, PMP_ENTRIES))
        io->print("[m09] WARNING: the PMP does not read back as before (pmpcfg0 0x%lx, was 0x%lx)", (unsigned long)now.cfg0,
                  (unsigned long)orig.cfg0);
    else
        io->print("[m09] entries restored to what the boot monitor left (read back equal)");

    for (unsigned i = 0; i < run->nlog; i++)
        io->print("[m09] trap %u: mcause %lu (%s access fault) mtval 0x%lx mepc 0x%lx", i, (unsigned long)run->log[i].cause,
                  run->log[i].cause == 5 ? "load" : run->log[i].cause == 7 ? "store" : "other", (unsigned long)run->log[i].tval,
                  (unsigned long)run->log[i].epc);
    io->print("[m09] own data word %lu -> %lu (allowed store), canary word 0x%lx -> 0x%lx (denied store)",
              (unsigned long)run->own_before, (unsigned long)run->own_after, (unsigned long)run->canary_before,
              (unsigned long)run->canary_after);
    if (run->ended == M09_ENDED_ABORT)
        io->print("[m09] user code stopped by an unexpected trap: mcause %lu mtval 0x%lx mepc 0x%lx", (unsigned long)run->abort_cause,
                  (unsigned long)run->abort_tval, (unsigned long)run->abort_epc);

    res = m09_judge(run->nlog, run->log, run->ended, run->probe_addr);
    if (res == M09_RES_OK && run->own_after != run->own_before + 1)
        res = M09_RES_NOT_EFFECTIVE;                       /* the allowed store did not happen: the fence is too tight, not a pass */
    io->print("[m09] result %s: %u of %u violations caught, supervisor continues", m09_result_name(res), run->nlog, M09_EXPECTED_FAULTS);
    return res;
}

#endif /* PFDAQ_PMP_H */
