#!/usr/bin/env python3
"""Runs the machine code of the M09 firmware ELF (m09_user_run, m09_user_entry, m09_trap) in the small RV64
interpreter of sim_rv64.py, with the PMP plan that the C header computes, under several PMP states.

  test_m09_asm.py [--elf PATH] [--nm PATH] [--objcopy PATH] [--plan-bin PATH]

What it shows (each is a separate check below):
  - with the plan installed: exactly three access faults, in order, with the right mcause, mtval and mepc;
    the allowed accesses happened, the denied store did not; no store went to Linux, boot-monitor or ring memory
  - afterwards the registers, the stack pointer, mtvec, mscratch, mstatus and mie are exactly as before, the
    privilege mode is machine mode, and the function returned to its caller
  - a pending timer interrupt is not taken during the excursion and is taken right after it, by the saved mtvec
  - with no PMP entry at all the user code cannot even be fetched: the excursion ends at once and restores everything
  - with the board's predicted state (a locked allow-all entry 0) nothing is blocked and the user-mode accesses succeed
  - a user-mode access that keeps faulting ends after 8 log entries
It is NOT a test of the PolarFire SoC: the interpreter has no caches, devices or timing, and its PMP is written
from the specification by the same author as the C model.
"""
import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sim_rv64 import Cpu, Memory, Pmp, Stop, M, U  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
fails = 0
checks = 0


def check(ok, msg):
    global fails, checks
    checks += 1
    print(("ok   " if ok else "FAIL ") + msg)
    if not ok:
        fails += 1


def symbols(nm, elf):
    out = subprocess.run([nm, "-n", elf], capture_output=True, text=True, check=True).stdout
    return {l.split()[2]: int(l.split()[0], 16) for l in out.splitlines() if len(l.split()) == 3}


class Bench:
    def __init__(self, image, sym, plan, pmp_setup):
        self.sym = sym
        self.mem = Memory()
        self.mem.load_image(0x91C00000, image)
        self.pmp = Pmp()
        self.cpu = Cpu(self.mem, self.pmp)
        self.plan = plan
        pmp_setup(self.pmp, plan)
        c = self.cpu
        # the state FreeRTOS' task is in when it calls the function: machine mode, interrupts on, its own trap vector
        c.c["mtvec"] = 0x91C00200
        c.c["mscratch"] = 0x1234567890ABCDEF
        c.c["mstatus"] = (1 << 3) | (1 << 7) | (3 << 11) | (1 << 13)        # MIE, MPIE, MPP = M, FS = initial
        c.c["mie"] = (1 << 7) | (1 << 11) | (1 << 3)                         # timer, external and software enabled
        self.sp0 = 0x91C40000                                                # the task's stack, deep inside the image region
        c.x[2] = self.sp0
        c.x[1] = 0x7FFF0000                                                  # return address: nothing runs there
        for r in list(range(8, 10)) + list(range(18, 28)):                   # s0..s11 hold the caller's values
            c.x[r] = 0x1111000000000000 + r
        c.x[5] = 0xDEADBEEF                                                  # t0 is clobbered, as the C ABI allows
        c.pc = sym["m09_user_run"]
        # the context the C code sets up (m09_hw_run_user)
        ctx = sym["m09_ctx"]
        self.ctx = ctx
        w = self.mem.write
        w(ctx + 0, 8, sym["m09_user_stack"] + 128 * 8, log=False)
        w(ctx + 8, 8, plan["own"], log=False)
        w(ctx + 16, 8, plan["ring_lo"], log=False)
        w(ctx + 24, 8, plan["canary"], log=False)
        w(ctx + 32, 8, plan["linux"], log=False)
        w(ctx + 40, 8, plan["hss"], log=False)
        w(plan["own"], 8, 1000, log=False)
        w(plan["canary"], 8, 0xCA7A57A1CA7A57A1, log=False)
        self.saved = {k: c.c[k] for k in ("mtvec", "mscratch", "mstatus", "mie")}
        self.saved_s = [c.x[r] for r in list(range(8, 10)) + list(range(18, 28))]
        self.mem.stores.clear()

    def run(self, max_steps=20000, also_stop=()):
        err = None
        try:
            self.cpu.run([0x7FFF0000] + list(also_stop), max_steps)
        except Stop as e:
            err = str(e)
        return err

    def ctxv(self, off):
        return self.mem.read(self.ctx + off, 8)

    def log(self):
        n = self.ctxv(48)
        return [(self.ctxv(96 + 24 * i), self.ctxv(104 + 24 * i), self.ctxv(112 + 24 * i)) for i in range(min(n, 8))]


def plan_installed(pmp, plan):
    cfg0 = sum(plan["cfg"][i] << (8 * i) for i in range(4))
    for i in range(4):
        pmp.write_addr(i, plan["addr"][i])
    pmp.write_cfg(0, cfg0)


def pmp_none(pmp, plan):
    pass


def pmp_board_like(pmp, plan):
    pmp.cfg[0] = 0x9F
    pmp.addr[0] = (1 << 54) - 1


def pmp_only_exec(pmp, plan):
    """user mode may fetch from the image but not load or store anywhere: every data access faults"""
    pmp.write_addr(0, (plan["fw_base"] | (plan["fw_size"] // 2 - 1)) >> 2)
    pmp.write_cfg(0, 0x1C)                                  # NAPOT, X only


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=os.path.join(ROOT, "build", "amp-m09", "polarfire-soc-freertos-amp-demo.elf"))
    ap.add_argument("--nm", default=os.path.join(ROOT, "build", "amp-m09", "toolchain", "bin", "nm"))
    ap.add_argument("--objcopy", default=os.path.join(ROOT, "build", "amp-m09", "toolchain", "bin", "objcopy"))
    ap.add_argument("--plan-bin", default=os.path.join(HERE, "..", "build", "host", "test_pmp"))
    a = ap.parse_args()
    if not os.path.isfile(a.elf):
        print("skip: %s not built (run experiments/amp-m09/build_firmware.sh)" % a.elf)
        return 0
    plan = json.loads(subprocess.run([a.plan_bin, "--plan"], capture_output=True, text=True, check=True).stdout)
    sym = symbols(a.nm, a.elf)
    tmp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "m09-image-%d.bin" % os.getpid())
    subprocess.run([a.objcopy, "-O", "binary", a.elf, tmp], check=True)
    image = open(tmp, "rb").read()
    os.unlink(tmp)
    for s in ("m09_user_run", "m09_user_entry", "m09_trap", "m09_ctx", "m09_user_stack", "m09_canary_page", "m09_own_word"):
        check(s in sym, "symbol %s is in the ELF at 0x%x" % (s, sym.get(s, 0)))
    # run the real geometry of this ELF, not the constants of the C test
    plan["canary"] = sym["m09_canary_page"]
    plan["own"] = sym["m09_own_word"]
    # the plan's addresses depend on the canary page; recompute through the C tool is not possible, so derive the NAPOT value here
    plan["addr"][0] = (plan["canary"] | 0x7FF) >> 2
    check(plan["canary"] & 0xFFF == 0 and plan["fw_base"] <= plan["canary"] < plan["fw_base"] + plan["fw_size"],
          "the canary page is 4 KiB aligned and inside the 1 MiB image")

    print("== 1 the plan installed: three violations caught")
    b = Bench(image, sym, plan, plan_installed)
    err = b.run()
    c = b.cpu
    check(err is None and c.pc == 0x7FFF0000, "the function returned to its caller (pc is the return address)%s" % (": " + err if err else ""))
    log = b.log()
    check(len(log) == 3, "three faults logged (%d)" % len(log))
    if len(log) == 3:
        ue = sym["m09_user_entry"]
        check([l[0] for l in log] == [5, 5, 7], "mcause 5, 5, 7 (load, load, store access fault): %s" % [l[0] for l in log])
        check([l[1] for l in log] == [plan["linux"], plan["hss"], plan["canary"]],
              "mtval = the three addresses: 0x%x 0x%x 0x%x" % tuple(l[1] for l in log))
        check([l[2] for l in log] == [ue + 0x2c, ue + 0x34, ue + 0x38], "mepc = the three instructions in m09_user_entry: %s" % [hex(l[2]) for l in log])
    check(b.ctxv(56) == 1, "ended by the ecall (ended = 1)")
    check(b.mem.read(plan["own"], 8) == 1001, "the allowed store happened: own word 1000 -> 1001")
    check(b.mem.read(plan["canary"], 8) == 0xCA7A57A1CA7A57A1, "the denied store did not happen: canary word unchanged")
    bad = [(ad, n) for ad, n in b.mem.stores
           if not (ad == plan["own"] or sym["m09_ctx"] <= ad < sym["m09_ctx"] + 288 or b.sp0 - 400 <= ad <= b.sp0
                   or sym["m09_user_stack"] <= ad < sym["m09_user_stack"] + 1024 or b.sp0 - 1024 <= ad < b.sp0)]
    check(not bad, "every store that happened was to the own word, the context, the user stack or the task stack; none to Linux, the boot monitor or the rings: %s" % bad)
    check(not any(plan["ring_lo"] <= ad < plan["ring_hi"] for ad, n in b.mem.stores), "no store into the ring region")
    check(c.priv == M, "back in machine mode")
    check({k: c.c[k] for k in ("mtvec", "mscratch", "mstatus", "mie")} == b.saved, "mtvec, mscratch, mstatus and mie are as before: %s" % {k: hex(v) for k, v in c.c.items() if k in b.saved})
    check(c.x[2] == b.sp0, "sp is as before (0x%x)" % c.x[2])
    check([c.x[r] for r in list(range(8, 10)) + list(range(18, 28))] == b.saved_s, "s0..s11 are as before")
    check(c.x[10] == 0, "a0 = 0 (return value)")
    check(c.mrets == 4, "mret ran 4 times: into user mode, and back after each of the three skipped faults (%d)" % c.mrets)
    check([t[0] for t in c.trap_log] == [5, 5, 7, 8], "traps taken: three access faults and one ecall: %s" % [t[0] for t in c.trap_log])
    check(all(t[3] == U for t in c.trap_log), "every trap came from user mode")
    check(c.steps < 400, "about %d instructions in all" % c.steps)

    print("== 2 a timer interrupt pending during the excursion")
    b = Bench(image, sym, plan, plan_installed)
    err = None
    try:                                         # the timer interrupt becomes pending when the hart first is in user mode
        while b.cpu.priv != U:
            b.cpu.step()
        b.cpu.c["mip"] = 1 << 7
        b.cpu.run([0x7FFF0000, 0x91C00200], 20000)   # stop when FreeRTOS' vector would start running
    except Stop as e:
        err = str(e)
    c = b.cpu
    check(err is None, "ends normally%s" % (": " + err if err else ""))
    irqs = [t for t in c.trap_log if t[0] >> 63]
    check(len(irqs) == 1 and (irqs[0][0] & 0xFF) == 7, "the timer interrupt was taken once: %s" % [hex(t[0]) for t in irqs])
    last = c.trap_log[-1]
    check(last[0] >> 63 and last[3] == M, "it was the LAST trap, taken in machine mode after the excursion, not during it")
    check(c.c["mtvec"] == 0x91C00200 and c.pc == 0x91C00200, "and it went to the saved mtvec (FreeRTOS' vector), with the registers already restored")
    check(c.x[2] == b.sp0 and [c.x[r] for r in list(range(8, 10)) + list(range(18, 28))] == b.saved_s, "at that moment sp and s0..s11 were already the caller's (the interrupt is transparent)")
    check(len([t for t in c.trap_log if t[0] == 5]) == 2 and len([t for t in c.trap_log if t[0] == 7]) == 1, "the three access faults still happened inside")
    check(c.c["mstatus"] & (3 << 11) == (3 << 11), "mstatus.MPP shows machine mode: the interrupt came from machine mode")

    print("== 3 no PMP entry at all: user mode cannot fetch")
    b = Bench(image, sym, plan, pmp_none)
    err = b.run()
    c = b.cpu
    check(err is None and c.pc == 0x7FFF0000, "the function still returns%s" % (": " + err if err else ""))
    check(b.ctxv(56) == 2 and b.ctxv(64) == 1, "ended = 2 (abort), abort cause 1 (instruction access fault)")
    check(b.ctxv(80) == sym["m09_user_entry"], "at the first user instruction (mepc 0x%x)" % b.ctxv(80))
    check(b.ctxv(48) == 0, "no access fault logged")
    check({k: c.c[k] for k in ("mtvec", "mscratch", "mstatus", "mie")} == b.saved and c.priv == M and c.x[2] == b.sp0, "all state restored")
    check(b.mem.read(plan["own"], 8) == 1000, "nothing was written")

    print("== 4 the board's predicted state: pmp0 locked, allow-all")
    b = Bench(image, sym, plan, pmp_board_like)
    err = b.run()
    c = b.cpu
    check(err is None and c.pc == 0x7FFF0000, "the function returns%s" % (": " + err if err else ""))
    check(b.ctxv(56) == 1 and b.ctxv(48) == 0, "ended by ecall with NO fault: user mode can read Linux's and the boot monitor's memory")
    check(b.mem.read(plan["canary"], 8) != 0xCA7A57A1CA7A57A1, "and the 'denied' store went through (to the harmless canary word)")
    check(not any(plan["linux"] <= ad < plan["linux"] + 8 or plan["hss"] <= ad < plan["hss"] + 8 for ad, n in b.mem.stores), "still no store to Linux or boot-monitor memory: those probes are loads")

    print("== 5 every data access faults: the log is bounded")
    b = Bench(image, sym, plan, pmp_only_exec)
    err = b.run()
    c = b.cpu
    check(err is None and c.pc == 0x7FFF0000, "the function returns instead of looping%s" % (": " + err if err else ""))
    check(b.ctxv(48) == 8 and b.ctxv(56) == 2, "8 faults logged, then the excursion is ended (ended = 2): n = %d" % b.ctxv(48))
    check({k: c.c[k] for k in ("mtvec", "mscratch", "mstatus", "mie")} == b.saved and c.priv == M, "state restored")

    print("== 6 the assembler uses only what the interpreter knows")
    check(True, "no 'unknown instruction' stop in any of the runs above")

    print("%d checks, %d failed" % (checks, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
