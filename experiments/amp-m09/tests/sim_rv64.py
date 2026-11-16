#!/usr/bin/env python3
"""A small RV64 interpreter for the M09 tests: machine and user mode, the CSRs the demo touches, traps, mret,
and a PMP written from the privileged specification (separate from the C model in pfdaq_pmp.h, so the
two can disagree).

It runs the REAL machine code of the firmware ELF (m09_user_run, m09_user_entry, m09_trap), not a
re-statement of it. It is not a model of the PolarFire SoC: no caches, no devices, no timing, only the
instructions the demo's assembler uses (RV64I without compressed instructions, Zicsr, mret, ecall).
An instruction it does not know stops the run with an error, so a change of the assembler that adds one
fails the test instead of being skipped.
"""
import struct

M, S, U = 3, 1, 0
MASK64 = (1 << 64) - 1

CSR = {
    0x300: "mstatus", 0x304: "mie", 0x305: "mtvec", 0x340: "mscratch", 0x341: "mepc", 0x342: "mcause",
    0x343: "mtval", 0x344: "mip", 0x3A0: "pmpcfg0", 0x3A2: "pmpcfg2",
}
for _i in range(16):
    CSR[0x3B0 + _i] = "pmpaddr%d" % _i

MSTATUS_MIE, MSTATUS_MPIE = 1 << 3, 1 << 7
MPP_SHIFT = 11


def sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


class Trap(Exception):
    def __init__(self, cause, tval=0):
        self.cause, self.tval = cause, tval


class Stop(Exception):
    pass


class Pmp:
    """Entries 0..15; cfg bytes and 54-bit addresses; lowest matching entry decides (privileged spec 3.7)."""

    def __init__(self):
        self.cfg = [0] * 16
        self.addr = [0] * 16

    def write_cfg(self, reg, value):
        base = 0 if reg == 0 else 8
        for k in range(8):
            i = base + k
            if self.cfg[i] & 0x80:
                continue                       # locked: ignored
            b = (value >> (8 * k)) & 0xFF
            if (b & 3) == 2:                   # W without R is reserved: keep the old value
                continue
            self.cfg[i] = b

    def read_cfg(self, reg):
        base = 0 if reg == 0 else 8
        return sum(self.cfg[base + k] << (8 * k) for k in range(8))

    def write_addr(self, i, value):
        if self.cfg[i] & 0x80:
            return
        if i + 1 < 16 and self.cfg[i + 1] & 0x80 and (self.cfg[i + 1] & 0x18) == 0x08:
            return                             # frozen by a locked TOR entry above
        self.addr[i] = value & ((1 << 54) - 1)

    def rng(self, i):
        a, c = self.addr[i], self.cfg[i]
        mode = (c >> 3) & 3
        if mode == 0:
            return None
        if mode == 1:
            lo = (self.addr[i - 1] << 2) if i else 0
            hi = (a << 2)
            return (lo, hi) if hi > lo else None       # [lo, hi)
        if mode == 2:
            return (a << 2, (a << 2) + 4)
        k = 0
        while (a >> k) & 1:
            k += 1
        size = 1 << (k + 3)
        base = (a >> (k + 1) << (k + 1)) << 2
        return (base, base + size)

    def allowed(self, addr, n, kind, priv):
        """kind: 'r', 'w' or 'x'"""
        for i in range(16):
            r = self.rng(i)
            if r is None:
                continue
            lo, hi = r
            if addr + n <= lo or addr >= hi:
                continue
            if addr < lo or addr + n > hi:
                return False
            c = self.cfg[i]
            if priv == M and not c & 0x80:
                return True
            return bool(c & {"r": 1, "w": 2, "x": 4}[kind])
        return priv == M


class Memory:
    def __init__(self):
        self.pages = {}
        self.stores = []                       # (addr, size) of every store that happened

    def _page(self, a):
        return self.pages.setdefault(a >> 12, bytearray(4096))

    def read(self, a, n):
        out = bytearray()
        for k in range(n):
            out.append(self._page(a + k)[(a + k) & 0xFFF])
        return int.from_bytes(out, "little")

    def write(self, a, n, v, log=True):
        for k in range(n):
            self._page(a + k)[(a + k) & 0xFFF] = (v >> (8 * k)) & 0xFF
        if log:
            self.stores.append((a, n))

    def load_image(self, base, data):
        for k in range(0, len(data), 4096):
            chunk = data[k:k + 4096]
            self.pages.setdefault((base + k) >> 12, bytearray(4096))[:len(chunk)] = chunk


class Cpu:
    def __init__(self, mem, pmp):
        self.x = [0] * 32
        self.pc = 0
        self.priv = M
        self.mem, self.pmp = mem, pmp
        self.c = {k: 0 for k in ("mstatus", "mie", "mtvec", "mscratch", "mepc", "mcause", "mtval", "mip")}
        self.steps = 0
        self.trap_log = []                     # (cause, mepc, mtval, priv at the time) of every trap taken
        self.mrets = 0

    # ---- csr
    def csr_read(self, n):
        name = CSR.get(n)
        if name is None:
            raise Stop("unknown CSR 0x%x" % n)
        if name.startswith("pmpcfg"):
            return self.pmp.read_cfg(0 if name == "pmpcfg0" else 2)
        if name.startswith("pmpaddr"):
            return self.pmp.addr[int(name[7:])]
        return self.c[name]

    def csr_write(self, n, v):
        name = CSR.get(n)
        if name is None:
            raise Stop("unknown CSR 0x%x" % n)
        v &= MASK64
        if name.startswith("pmpcfg"):
            self.pmp.write_cfg(0 if name == "pmpcfg0" else 2, v)
        elif name.startswith("pmpaddr"):
            self.pmp.write_addr(int(name[7:]), v)
        else:
            self.c[name] = v

    # ---- memory with checks
    def load(self, a, n):
        if not self.pmp.allowed(a, n, "r", self.priv):
            raise Trap(5, a)
        return self.mem.read(a, n)

    def store(self, a, n, v):
        if not self.pmp.allowed(a, n, "w", self.priv):
            raise Trap(7, a)
        self.mem.write(a, n, v)

    def fetch(self):
        if not self.pmp.allowed(self.pc, 4, "x", self.priv):
            raise Trap(1, self.pc)
        return self.mem.read(self.pc, 4)

    # ---- traps
    def take_trap(self, cause, tval, interrupt=False):
        st = self.c["mstatus"]
        self.c["mepc"] = self.pc
        self.c["mcause"] = cause | ((1 << 63) if interrupt else 0)
        self.c["mtval"] = tval
        mpie = MSTATUS_MPIE if st & MSTATUS_MIE else 0
        st = (st & ~(MSTATUS_MIE | MSTATUS_MPIE | (3 << MPP_SHIFT))) | mpie | (self.priv << MPP_SHIFT)
        self.c["mstatus"] = st
        self.trap_log.append((self.c["mcause"], self.pc, tval, self.priv))
        self.priv = M
        self.pc = self.c["mtvec"] & ~3

    def pending_interrupt(self):
        p = self.c["mip"] & self.c["mie"]
        if not p:
            return None
        if self.priv == M and not self.c["mstatus"] & MSTATUS_MIE:
            return None
        for bit in (11, 3, 7):
            if p >> bit & 1:
                return bit
        return None

    # ---- execute
    def step(self):
        irq = self.pending_interrupt()
        if irq is not None:
            self.take_trap(irq, 0, interrupt=True)
            return
        try:
            ins = self.fetch()
            if ins & 3 != 3:
                raise Stop("compressed instruction at 0x%x (the demo's assembler is built without them)" % self.pc)
            self.exec(ins)
        except Trap as t:
            self.take_trap(t.cause, t.tval)
        self.x[0] = 0
        self.steps += 1

    def exec(self, i):
        op = i & 0x7F
        rd, f3, rs1, rs2, f7 = (i >> 7) & 31, (i >> 12) & 7, (i >> 15) & 31, (i >> 20) & 31, i >> 25
        x = self.x
        nxt = (self.pc + 4) & MASK64
        imm_i = sx(i >> 20, 12)
        if op == 0x37:
            x[rd] = sx(i & 0xFFFFF000, 32) & MASK64
        elif op == 0x17:
            x[rd] = (self.pc + sx(i & 0xFFFFF000, 32)) & MASK64
        elif op == 0x6F:
            imm = sx(((i >> 31) << 20) | (((i >> 12) & 0xFF) << 12) | (((i >> 20) & 1) << 11) | (((i >> 21) & 0x3FF) << 1), 21)
            x[rd] = nxt
            nxt = (self.pc + imm) & MASK64
        elif op == 0x67:
            t = (x[rs1] + imm_i) & ~1 & MASK64
            x[rd] = nxt
            nxt = t
        elif op == 0x63:
            imm = sx(((i >> 31) << 12) | (((i >> 7) & 1) << 11) | (((i >> 25) & 0x3F) << 5) | (((i >> 8) & 0xF) << 1), 13)
            a, b = x[rs1], x[rs2]
            sa, sb = sx(a, 64), sx(b, 64)
            take = {0: a == b, 1: a != b, 4: sa < sb, 5: sa >= sb, 6: a < b, 7: a >= b}[f3]
            if take:
                nxt = (self.pc + imm) & MASK64
        elif op == 0x03:
            a = (x[rs1] + imm_i) & MASK64
            n, signed = {0: (1, True), 1: (2, True), 2: (4, True), 3: (8, True), 4: (1, False), 5: (2, False), 6: (4, False)}[f3]
            v = self.load(a, n)
            x[rd] = (sx(v, 8 * n) if signed else v) & MASK64
        elif op == 0x23:
            imm = sx(((i >> 25) << 5) | ((i >> 7) & 31), 12)
            n = {0: 1, 1: 2, 2: 4, 3: 8}[f3]
            self.store((x[rs1] + imm) & MASK64, n, x[rs2])
        elif op == 0x13:
            a = x[rs1]
            if f3 == 0:
                r = a + imm_i
            elif f3 == 1:
                r = a << ((i >> 20) & 63)
            elif f3 == 7:
                r = a & (imm_i & MASK64)
            elif f3 == 6:
                r = a | (imm_i & MASK64)
            elif f3 == 4:
                r = a ^ (imm_i & MASK64)
            elif f3 == 5:
                sh = (i >> 20) & 63
                r = (sx(a, 64) >> sh) if (i >> 30) & 1 else (a >> sh)
            else:
                raise Stop("OP-IMM funct3 %d" % f3)
            x[rd] = r & MASK64
        elif op == 0x1B:
            if f3 == 0:
                x[rd] = sx(x[rs1] + imm_i, 32) & MASK64
            else:
                raise Stop("OP-IMM-32 funct3 %d" % f3)
        elif op == 0x33:
            a, b = x[rs1], x[rs2]
            if f7 == 0 and f3 == 0:
                r = a + b
            elif f7 == 0x20 and f3 == 0:
                r = a - b
            elif f7 == 0 and f3 == 7:
                r = a & b
            elif f7 == 0 and f3 == 6:
                r = a | b
            elif f7 == 0 and f3 == 4:
                r = a ^ b
            elif f7 == 0 and f3 == 1:
                r = a << (b & 63)
            else:
                raise Stop("OP f7 %d f3 %d" % (f7, f3))
            x[rd] = r & MASK64
        elif op == 0x0F:
            pass                               # fence
        elif op == 0x73:
            if f3 == 0:
                if i == 0x00000073:            # ecall
                    raise Trap({U: 8, S: 9, M: 11}[self.priv])
                if i == 0x30200073:            # mret
                    if self.priv != M:
                        raise Trap(2, i)
                    st = self.c["mstatus"]
                    self.priv = (st >> MPP_SHIFT) & 3
                    st = (st & ~MSTATUS_MIE) | (MSTATUS_MIE if st & MSTATUS_MPIE else 0) | MSTATUS_MPIE
                    st &= ~(3 << MPP_SHIFT)
                    self.c["mstatus"] = st
                    nxt = self.c["mepc"]
                    self.mrets += 1
                else:
                    raise Stop("system instruction 0x%08x" % i)
            else:
                if self.priv != M:
                    raise Trap(2, i)           # CSR access from user mode
                n = i >> 20
                old = self.csr_read(n)
                src = x[rs1]
                if f3 == 1:                    # csrrw
                    self.csr_write(n, src)
                elif f3 == 2:                  # csrrs
                    if rs1:
                        self.csr_write(n, old | src)
                elif f3 == 3:                  # csrrc
                    if rs1:
                        self.csr_write(n, old & ~src)
                else:
                    raise Stop("csr form %d" % f3)
                x[rd] = old
        else:
            raise Stop("opcode 0x%02x (instruction 0x%08x at 0x%x)" % (op, i, self.pc))
        self.pc = nxt

    def run(self, until_pc, max_steps=100000):
        stops = until_pc if isinstance(until_pc, (list, tuple, set)) else (until_pc,)
        while self.pc not in stops:
            if self.steps >= max_steps:
                raise Stop("did not finish in %d instructions" % max_steps)
            self.step()
