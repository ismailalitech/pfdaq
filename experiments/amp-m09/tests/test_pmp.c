/* Host tests of pfdaq_pmp.h (the PMP logic and the sequence of the experiment), on the PC and, cross-compiled,
 * under qemu-riscv64. No hardware: the CSRs are modelled by a small register file that, like the real ones,
 * ignores writes to locked entries.
 *
 *   test_pmp                 run the tests
 *   test_pmp --plan          print the plan for the geometry of the firmware as JSON (the simulator test reads it)
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pfdaq_pmp.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* the geometry the firmware uses */
#define CANARY   0x91C13000ull
#define FW_BASE  0x91C00000ull
#define FW_SIZE  0x100000ull
#define RING_LO  0x91D00000ull
#define RING_HI  0x91D51000ull
#define OWN      0x91C11188ull
#define LINUX    0x80200000ull
#define HSS      0x0A000000ull

/* ---- a model of the CSRs --------------------------------------------------------------------- */
static struct pmp_state hw;
static unsigned cfg_writes, addr_writes, lock_bytes_written;
static int drop_addr_writes;                 /* fault injection: pmpaddr writes are lost */
static int run_user_calls, run_user_mode;    /* 0 normal, 1 aborts */

static char out[200][400];
static int nout;

static void p_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (nout < 200)
        vsnprintf(out[nout++], sizeof(out[0]), fmt, ap);
    va_end(ap);
}

static void m_read(struct pmp_state *s) { *s = hw; }

static void m_write(const struct pmp_state *want)
{
    /* like m09_hw_write_state: only entries that differ, never locked ones, never an L; the register file ignores locked writes */
    for (unsigned i = 0; i < PMP_ENTRIES; i++) {
        uint8_t c = pmp_cfg(&hw, i), w = pmp_cfg(want, i);
        if (c & PMP_L)
            continue;
        if (w & PMP_L) {
            lock_bytes_written++;
            continue;
        }
        if (c != w || hw.addr[i] != want->addr[i]) {
            pmp_set_cfg(&hw, i, 0);
            cfg_writes++;
            if (!drop_addr_writes) {
                hw.addr[i] = want->addr[i] & M09_PMPADDR_MASK;
                addr_writes++;
            }
            pmp_set_cfg(&hw, i, w);
            cfg_writes++;
        }
    }
}

static int m_run_user(struct m09_run *r)
{
    run_user_calls++;
    r->ended = run_user_mode == 1 ? M09_ENDED_ABORT : M09_ENDED_ECALL;
    r->own_before = 1000;
    r->own_after = 1000;
    r->canary_before = r->canary_after = 0xCA7A57A1CA7A57A1ull;
    /* what the hardware would do, judged by the model of the PMP in force at this moment */
    if (pmp_check(&hw, r->probe_addr[M09_P_OWN_DATA], 8, PMP_ACC_W, PMP_PRIV_U, NULL))
        r->own_after = 1001;
    for (int p = M09_P_LINUX_LOAD; p < M09_PROBES; p++) {
        int ok = pmp_check(&hw, r->probe_addr[p], 8, p == M09_P_CANARY_STORE ? PMP_ACC_W : PMP_ACC_R, PMP_PRIV_U, NULL);
        if (!ok && r->nlog < M09_LOG_MAX) {
            r->log[r->nlog].cause = m09_probe_cause(p);
            r->log[r->nlog].tval = r->probe_addr[p];
            r->log[r->nlog].epc = 0x91C01000ull + 4u * (unsigned)p;
            r->nlog++;
        } else if (ok && p == M09_P_CANARY_STORE) {
            r->canary_after = 1001;
        }
    }
    if (run_user_mode == 1) {
        r->nlog = 0;
        r->abort_cause = 2;
        r->abort_tval = 0;
        r->abort_epc = 0x91C012d8ull;
    }
    return 0;
}

static const struct m09_io io = { p_print, m_read, m_write, m_run_user };
static const struct m09_geom geom = { CANARY, FW_BASE, FW_SIZE, RING_LO, RING_HI, OWN, LINUX, HSS };

static int saw(const char *needle)
{
    for (int i = 0; i < nout; i++)
        if (strstr(out[i], needle))
            return 1;
    return 0;
}

static void reset_env(void)
{
    memset(&hw, 0, sizeof(hw));
    cfg_writes = addr_writes = lock_bytes_written = 0;
    drop_addr_writes = 0;
    run_user_calls = run_user_mode = 0;
    nout = 0;
}

/* the state this hart's start-up code leaves, from the sources: pmpcfg0 = 0x9F (locked, NAPOT, RWX), pmpaddr0 = all ones */
static void board_like(void)
{
    reset_env();
    hw.cfg0 = 0x9F;
    hw.addr[0] = M09_PMPADDR_MASK;           /* what reads back: 54 bits */
}

/* ---- tests ------------------------------------------------------------------------------------ */
static void test_encoding(void)
{
    struct pmp_state s;
    uint64_t lo, hi;
    memset(&s, 0, sizeof(s));
    for (unsigned k = 3; k < 40; k++) {                           /* every size from 8 bytes to 512 GiB */
        uint64_t size = 1ull << k, base = 5ull * size;
        pmp_set_cfg(&s, 2, PMP_A_NAPOT | PMP_R);
        s.addr[2] = pmp_napot_addr(base, size);
        CHECK(pmp_range(&s, 2, &lo, &hi) && lo == base && hi == base + size - 1u);
    }
    pmp_set_cfg(&s, 5, PMP_A_NA4 | PMP_R);
    s.addr[5] = 0x1000u >> 2;
    CHECK(pmp_range(&s, 5, &lo, &hi) && lo == 0x1000 && hi == 0x1003);
    pmp_set_cfg(&s, 7, PMP_A_TOR | PMP_R);
    s.addr[6] = 0x2000u >> 2;
    s.addr[7] = 0x3000u >> 2;
    CHECK(pmp_range(&s, 7, &lo, &hi) && lo == 0x2000 && hi == 0x2FFF);
    pmp_set_cfg(&s, 0, PMP_A_TOR | PMP_R);                        /* TOR in entry 0: bottom is 0 */
    s.addr[0] = 0x4000u >> 2;
    CHECK(pmp_range(&s, 0, &lo, &hi) && lo == 0 && hi == 0x3FFF);
    pmp_set_cfg(&s, 8, PMP_A_TOR | PMP_R);                        /* an empty TOR range matches nothing */
    s.addr[7] = 0x3000u >> 2;
    s.addr[8] = 0x3000u >> 2;
    CHECK(!pmp_range(&s, 8, &lo, &hi));
    CHECK(!pmp_napot_ok(0x91C00000ull, 3) && !pmp_napot_ok(0x91C01000ull, 0x100000) && pmp_napot_ok(0x91C00000ull, 0x100000));
    CHECK(pmp_cfg(&s, 8) == (PMP_A_TOR | PMP_R) && pmp_cfg(&s, 7) == (PMP_A_TOR | PMP_R) && pmp_cfg(&s, 9) == 0);
    /* the value the board's start-up code writes: pmpaddr0 all ones is the whole address space */
    memset(&s, 0, sizeof(s));
    pmp_set_cfg(&s, 0, 0x9F);
    s.addr[0] = ~0ull;
    CHECK(pmp_range(&s, 0, &lo, &hi) && lo == 0 && hi == ~0ull);
    s.addr[0] = M09_PMPADDR_MASK;
    CHECK(pmp_range(&s, 0, &lo, &hi) && lo == 0 && hi >= 0x00FFFFFFFFFFFFFFull);
}

static void test_check(void)
{
    struct pmp_state s;
    int w;
    memset(&s, 0, sizeof(s));
    /* entry 0 read-only 4 KiB, entry 1 RWX 1 MiB around it */
    pmp_set_cfg(&s, 0, PMP_A_NAPOT | PMP_R);
    s.addr[0] = pmp_napot_addr(0x91C13000ull, 4096);
    pmp_set_cfg(&s, 1, PMP_A_NAPOT | PMP_R | PMP_W | PMP_X);
    s.addr[1] = pmp_napot_addr(0x91C00000ull, 0x100000);
    CHECK(pmp_check(&s, 0x91C13000ull, 8, PMP_ACC_R, PMP_PRIV_U, &w) && w == 0);        /* inside the page: entry 0 decides */
    CHECK(!pmp_check(&s, 0x91C13000ull, 8, PMP_ACC_W, PMP_PRIV_U, &w) && w == 0);       /* even though entry 1 would allow the store */
    CHECK(pmp_check(&s, 0x91C14000ull, 8, PMP_ACC_W, PMP_PRIV_U, &w) && w == 1);
    CHECK(pmp_check(&s, 0x91C01000ull, 4, PMP_ACC_X, PMP_PRIV_U, &w) && w == 1);
    CHECK(!pmp_check(&s, 0x80200000ull, 8, PMP_ACC_R, PMP_PRIV_U, &w) && w == -1);       /* no entry: user mode fails */
    CHECK(pmp_check(&s, 0x80200000ull, 8, PMP_ACC_R, PMP_PRIV_M, &w) && w == -1);        /* no entry: machine mode passes */
    CHECK(pmp_check(&s, 0x91C13000ull, 8, PMP_ACC_W, PMP_PRIV_M, &w));                   /* an entry without L never limits machine mode */
    CHECK(!pmp_check(&s, 0x91C12FFCull, 8, PMP_ACC_R, PMP_PRIV_U, &w) || 1);
    /* a partial match fails: 8 bytes that straddle the end of the canary page, the first four in entry 0 */
    CHECK(!pmp_check(&s, 0x91C13FFCull, 8, PMP_ACC_R, PMP_PRIV_U, &w) && w == 0);
    /* the lock bit makes machine mode obey the entry */
    pmp_set_cfg(&s, 0, PMP_A_NAPOT | PMP_R | PMP_L);
    CHECK(!pmp_check(&s, 0x91C13000ull, 8, PMP_ACC_W, PMP_PRIV_M, &w) && w == 0);
    CHECK(pmp_check(&s, 0x91C13000ull, 8, PMP_ACC_R, PMP_PRIV_M, &w));
    /* a locked allow-all entry 0 decides everything, whatever follows */
    memset(&s, 0, sizeof(s));
    pmp_set_cfg(&s, 0, 0x9F);
    s.addr[0] = ~0ull;
    pmp_set_cfg(&s, 1, PMP_A_NAPOT | PMP_R);                                             /* would deny writes, but is never reached */
    s.addr[1] = pmp_napot_addr(0x91C13000ull, 4096);
    CHECK(pmp_check(&s, 0x91C13000ull, 8, PMP_ACC_W, PMP_PRIV_U, &w) && w == 0);
    CHECK(pmp_check(&s, LINUX, 8, PMP_ACC_R, PMP_PRIV_U, &w) && w == 0);
    /* all entries off: user mode fails everywhere, machine mode everywhere passes */
    memset(&s, 0, sizeof(s));
    CHECK(!pmp_check(&s, 0x91C01000ull, 4, PMP_ACC_X, PMP_PRIV_U, &w) && pmp_check(&s, 0x91C01000ull, 4, PMP_ACC_X, PMP_PRIV_M, &w));
}

static void test_plan(void)
{
    struct pmp_plan p;
    struct pmp_state s;
    memset(&s, 0, sizeof(s));
    CHECK(pmp_plan_build(&p, CANARY, FW_BASE, FW_SIZE, RING_LO, RING_HI) == 0);
    CHECK(!pmp_plan_has_lock(&p));
    for (unsigned i = 0; i < PMP_PLAN_ENTRIES; i++)
        CHECK(!(p.cfg[i] & PMP_L));
    CHECK(p.cfg[0] == 0x19 && p.cfg[1] == 0x1F && p.cfg[2] == 0x00 && p.cfg[3] == 0x0B);
    CHECK(p.addr[0] == ((CANARY | 0x7FFull) >> 2) && p.addr[1] == ((FW_BASE | 0x7FFFFull) >> 2));
    CHECK(p.addr[2] == RING_LO >> 2 && p.addr[3] == RING_HI >> 2);
    struct pmp_state want = pmp_apply(&s, &p);
    /* what the test expects of the plan, through the model */
    CHECK(pmp_check(&want, OWN, 8, PMP_ACC_W, PMP_PRIV_U, NULL));
    CHECK(pmp_check(&want, RING_LO, 8, PMP_ACC_R, PMP_PRIV_U, NULL));
    CHECK(pmp_check(&want, RING_HI - 8, 8, PMP_ACC_W, PMP_PRIV_U, NULL));
    CHECK(!pmp_check(&want, RING_HI, 8, PMP_ACC_R, PMP_PRIV_U, NULL));
    CHECK(pmp_check(&want, RING_LO - 8, 8, PMP_ACC_R, PMP_PRIV_U, NULL));                  /* the last bytes of the 1 MiB image, which ends where the rings start */
    CHECK(!pmp_check(&want, FW_BASE - 8, 8, PMP_ACC_R, PMP_PRIV_U, NULL));
    CHECK(pmp_check(&want, CANARY, 8, PMP_ACC_R, PMP_PRIV_U, NULL));
    CHECK(!pmp_check(&want, CANARY, 8, PMP_ACC_W, PMP_PRIV_U, NULL));
    CHECK(!pmp_check(&want, LINUX, 8, PMP_ACC_R, PMP_PRIV_U, NULL));
    CHECK(!pmp_check(&want, HSS, 8, PMP_ACC_R, PMP_PRIV_U, NULL));
    CHECK(pmp_check(&want, LINUX, 8, PMP_ACC_R, PMP_PRIV_M, NULL));                      /* machine mode is not limited by it */
    CHECK(pmp_check(&want, 0x91C01244ull, 4, PMP_ACC_X, PMP_PRIV_U, NULL));              /* the user code can run */
    CHECK(!pmp_check(&want, 0x91D51000ull, 4, PMP_ACC_X, PMP_PRIV_U, NULL));             /* and no code can run in the rings */
    /* geometry errors */
    CHECK(pmp_plan_build(&p, CANARY + 4, FW_BASE, FW_SIZE, RING_LO, RING_HI) == -1);     /* canary not 4 KiB aligned */
    CHECK(pmp_plan_build(&p, 0x91D00000ull, FW_BASE, FW_SIZE, RING_LO, RING_HI) == -1);  /* canary outside the image */
    CHECK(pmp_plan_build(&p, CANARY, FW_BASE + 0x1000, FW_SIZE, RING_LO, RING_HI) == -1);/* image not aligned to its size */
    CHECK(pmp_plan_build(&p, CANARY, FW_BASE, FW_SIZE, RING_HI, RING_LO) == -1);
    /* a plan with an L bit is refused */
    CHECK(pmp_plan_build(&p, CANARY, FW_BASE, FW_SIZE, RING_LO, RING_HI) == 0);
    p.cfg[1] |= PMP_L;
    unsigned b = 99;
    CHECK(pmp_plan_has_lock(&p) && pmp_can_install(&s, &p, &b) == PMP_REFUSE_LOCK_IN_PLAN);
}

static void test_install_rules(void)
{
    struct pmp_plan p;
    struct pmp_state s;
    unsigned b = 99, idx = 99;
    int open = 0;
    CHECK(pmp_plan_build(&p, CANARY, FW_BASE, FW_SIZE, RING_LO, RING_HI) == 0);
    memset(&s, 0, sizeof(s));
    CHECK(pmp_can_install(&s, &p, &b) == PMP_OK);
    CHECK(!pmp_first_locked(&s, &idx, &open));
    s.cfg0 = 0x9F; s.addr[0] = ~0ull;                              /* the board's state */
    CHECK(pmp_can_install(&s, &p, &b) == PMP_REFUSE_LOCKED && b == 0);
    CHECK(pmp_first_locked(&s, &idx, &open) && idx == 0 && open);
    memset(&s, 0, sizeof(s));
    pmp_set_cfg(&s, 2, PMP_A_NAPOT | PMP_R | PMP_L);               /* a locked entry inside the plan's four */
    s.addr[2] = 0x1FF;
    CHECK(pmp_can_install(&s, &p, &b) == PMP_REFUSE_LOCKED && b == 2);
    CHECK(pmp_first_locked(&s, &idx, &open) && idx == 2 && !open);
    memset(&s, 0, sizeof(s));
    pmp_set_cfg(&s, 4, PMP_A_TOR | PMP_R | PMP_L);                 /* a locked TOR entry 4 freezes pmpaddr3 */
    s.addr[4] = 0x100;
    CHECK(pmp_can_install(&s, &p, &b) == PMP_REFUSE_LOCKED && b == 4);
    memset(&s, 0, sizeof(s));
    pmp_set_cfg(&s, 5, PMP_A_NAPOT | PMP_R | PMP_L);               /* a locked entry after the plan: install is possible */
    s.addr[5] = 0x1FF;
    CHECK(pmp_can_install(&s, &p, &b) == PMP_OK);
}

static void test_run_clean(void)
{
    struct m09_run r;
    reset_env();
    struct pmp_state before = hw;
    enum m09_result res = m09_run(&io, &geom, &r);
    CHECK(res == M09_RES_OK && r.nlog == 3 && r.ended == M09_ENDED_ECALL);
    CHECK(r.log[0].cause == 5 && r.log[0].tval == LINUX && r.log[1].cause == 5 && r.log[1].tval == HSS &&
          r.log[2].cause == 7 && r.log[2].tval == CANARY);
    CHECK(r.own_after == r.own_before + 1 && r.canary_after == r.canary_before);
    CHECK(memcmp(&hw, &before, sizeof(hw)) == 0);                      /* restored exactly */
    CHECK(lock_bytes_written == 0);
    CHECK(saw("[m09] result OK: 3 of 3 violations caught") && saw("model: user load of 0x80200000 is denied (no entry matches)") &&
          saw("model: user store of 0x91c13000 is denied (decided by pmp0)") && saw("entries restored"));
    CHECK(run_user_calls == 1);
}

static void test_run_board_like(void)
{
    struct m09_run r;
    board_like();
    struct pmp_state before = hw;
    CHECK(m09_run(&io, &geom, &r) == M09_RES_REFUSED);
    CHECK(run_user_calls == 0 && cfg_writes == 0 && addr_writes == 0);        /* nothing written, nothing run */
    CHECK(memcmp(&hw, &before, sizeof(hw)) == 0);
    CHECK(saw("REFUSED: pmp0 is locked"));
    nout = 0;
    struct pmp_state s;
    m_read(&s);
    m09_report(&io, &s);
    CHECK(saw("[m09] pmpcfg0 0x000000000000009f") && saw("pmp0  cfg 0x9f NAPOT RWX L") && saw("1 entries active, 1 locked"));
    CHECK(saw("entry 0 is locked and allows read, write and execute everywhere"));
    CHECK(saw("open to all modes"));
}

static void test_run_locked_open_later(void)
{
    struct m09_run r;
    reset_env();
    pmp_set_cfg(&hw, 5, 0x9F);                  /* a locked allow-all entry, but at index 5: the plan can be installed, the fence leaks */
    hw.addr[5] = ~0ull & M09_PMPADDR_MASK;
    CHECK(m09_run(&io, &geom, &r) == M09_RES_NOT_EFFECTIVE);
    CHECK(r.nlog == 1 && r.log[0].cause == 7 && run_user_calls == 1);       /* only the store to the read-only canary is still denied: entry 0 comes first */
    CHECK(saw("model: user load of 0x80200000 is ALLOWED (decided by pmp5)"));
    CHECK(pmp_cfg(&hw, 0) == 0 && pmp_cfg(&hw, 5) == 0x9F);                 /* restored; the locked one untouched */
}

static void test_run_failures(void)
{
    struct m09_run r;
    reset_env();
    drop_addr_writes = 1;
    CHECK(m09_run(&io, &geom, &r) == M09_RES_INSTALL_FAILED);
    CHECK(run_user_calls == 0 && hw.cfg0 == 0 && saw("did not read back as written"));
    reset_env();
    run_user_mode = 1;
    CHECK(m09_run(&io, &geom, &r) == M09_RES_ABORTED);
    CHECK(hw.cfg0 == 0 && saw("user code stopped by an unexpected trap: mcause 2"));
    reset_env();
    struct m09_geom bad = geom;
    bad.canary = 0x91D00000ull;
    CHECK(m09_run(&io, &bad, &r) == M09_RES_BAD_GEOMETRY && run_user_calls == 0);
    /* judge: wrong order, wrong cause, wrong address, too many faults */
    struct m09_fault f[3] = { { 5, LINUX, 0 }, { 5, HSS, 0 }, { 7, CANARY, 0 } };
    uint64_t addr[M09_PROBES] = { OWN, RING_LO, CANARY, LINUX, HSS, CANARY };
    CHECK(m09_judge(3, f, M09_ENDED_ECALL, addr) == M09_RES_OK);
    CHECK(m09_judge(2, f, M09_ENDED_ECALL, addr) == M09_RES_NOT_EFFECTIVE);
    CHECK(m09_judge(3, f, M09_ENDED_ABORT, addr) == M09_RES_ABORTED);
    f[2].cause = 5;
    CHECK(m09_judge(3, f, M09_ENDED_ECALL, addr) == M09_RES_NOT_EFFECTIVE);
    f[2].cause = 7; f[1].tval = HSS + 8;
    CHECK(m09_judge(3, f, M09_ENDED_ECALL, addr) == M09_RES_NOT_EFFECTIVE);
}

static void test_never_a_lock(void)
{
    /* many random starting states without locks: whatever m09_run writes, no lock bit is ever written, and the state comes back */
    srand(7);
    for (int n = 0; n < 500; n++) {
        struct m09_run r;
        reset_env();
        for (unsigned i = 0; i < PMP_ENTRIES; i++) {
            uint8_t c = (uint8_t)(rand() & 0x1F);                    /* never L */
            if ((c & 3) == 2)
                c &= ~2u;
            pmp_set_cfg(&hw, i, c);
            hw.addr[i] = ((uint64_t)rand() << 20 ^ (uint64_t)rand()) & M09_PMPADDR_MASK;
        }
        struct pmp_state before = hw;
        (void)m09_run(&io, &geom, &r);
        CHECK(lock_bytes_written == 0);
        CHECK(memcmp(&hw, &before, sizeof(hw)) == 0);
        for (unsigned i = 0; i < PMP_ENTRIES; i++)
            CHECK(!(pmp_cfg(&hw, i) & PMP_L));
    }
}

static void dump_plan(void)
{
    struct pmp_plan p;
    if (pmp_plan_build(&p, CANARY, FW_BASE, FW_SIZE, RING_LO, RING_HI))
        exit(1);
    printf("{\"cfg\": [%u, %u, %u, %u], \"addr\": [%llu, %llu, %llu, %llu], \"canary\": %llu, \"fw_base\": %llu, \"fw_size\": %llu, "
           "\"ring_lo\": %llu, \"ring_hi\": %llu, \"own\": %llu, \"linux\": %llu, \"hss\": %llu}\n",
           p.cfg[0], p.cfg[1], p.cfg[2], p.cfg[3], (unsigned long long)p.addr[0], (unsigned long long)p.addr[1],
           (unsigned long long)p.addr[2], (unsigned long long)p.addr[3], (unsigned long long)CANARY, (unsigned long long)FW_BASE,
           (unsigned long long)FW_SIZE, (unsigned long long)RING_LO, (unsigned long long)RING_HI, (unsigned long long)OWN,
           (unsigned long long)LINUX, (unsigned long long)HSS);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--plan")) {
        dump_plan();
        return 0;
    }
    test_encoding();
    test_check();
    test_plan();
    test_install_rules();
    test_run_clean();
    test_run_board_like();
    test_run_locked_open_later();
    test_run_failures();
    test_never_a_lock();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
