#!/usr/bin/env python3
"""pfdaq_capture - run on the board (Linux, root). Arms the CoreAXI4DMAController stream
descriptor, starts the fabric frame source (pfdaq_stream_src) and verifies what lands in
non-cached DDR. Prints one JSON line with the result.

Addresses (reference design v2026.04 + project integration):
  0x40000F00  pfdaq_stream_src APB registers (FIC3)
  0x60010000  CoreAXI4DMAController registers (FIC0), stream descriptor address reg 0 at 0x460
  0xC8000000  non-cached DDR window, reserved in the device tree (buffer@c8000000, 32 MiB)
Stream descriptor (user guide DS50003714A, fig. 1-3): +0 config, +4 byte count, +8 destination.
"""
import argparse, ctypes, json, mmap, os, select, struct, sys, time, zlib

GEN_BASE, GEN_OFF = 0x40000000, 0xF00
DMA_BASE = 0x60010000
DDR_BASE, DDR_SPAN = 0xC8000000, 0x02000000
DESC_OFF, BUF_OFF = 0x0, 0x1000
ID_VALUE, MAGIC = 0x50464451, 0xA55A
GUARD = 0xDEADBEEF
# descriptor config: bit3 valid, bit2 destination data ready, [1:0]=01 incrementing address
DESC_CFG = 0xD
STAT, MASK, CLEAR, EXT_ADDR, STREAM0 = 0x10, 0x14, 0x18, 0x1C, 0x460


_lib = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'libmmio.so'))
_lib.mmio_rd32.restype = ctypes.c_uint32
_lib.mmio_rd32.argtypes = [ctypes.c_void_p]
_lib.mmio_wr32.argtypes = [ctypes.c_void_p, ctypes.c_uint32]


class Regs:
    """32-bit register access through /dev/mem using libmmio.so (see mmio.c): Python's own
    mmap/ctypes stores are done with memcpy, byte by byte on this target."""
    def __init__(self, fd, base, size):
        self.m = mmap.mmap(fd, size, offset=base)
        self.addr = ctypes.addressof(ctypes.c_char.from_buffer(self.m))
    def rd(self, off):
        return _lib.mmio_rd32(self.addr + off)
    def wr(self, off, val):
        _lib.mmio_wr32(self.addr + off, val & 0xFFFFFFFF)


def find_uio(name):
    for d in sorted(os.listdir('/sys/class/uio')):
        if open('/sys/class/uio/%s/name' % d).read().strip() == name:
            return '/dev/' + d
    raise SystemExit('uio device %s not found' % name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--words', type=int, default=16, help='payload words per frame')
    ap.add_argument('--frames', type=int, default=8)
    ap.add_argument('--per-frame', action='store_true', help='one DMA transaction per frame, software-paced')
    ap.add_argument('--fault', default='none', choices=['none', 'crc', 'seq', 'payload'])
    ap.add_argument('--fault-frame', type=int, default=0)
    ap.add_argument('--timeout', type=float, default=5.0)
    ap.add_argument('--dump', type=int, default=0, help='print first N words')
    a = ap.parse_args()

    fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
    gen = Regs(fd, GEN_BASE, 0x1000)
    dma = Regs(fd, DMA_BASE, 0x1000)
    ddr = Regs(fd, DDR_BASE, DDR_SPAN)
    g = lambda o: gen.rd(GEN_OFF + o)
    gw = lambda o, v: gen.wr(GEN_OFF + o, v)

    res = dict(args=vars(a), errors=[])
    err = res['errors'].append
    res['id'] = '%08x' % g(0x00)
    res['version'] = '%08x' % g(0x04)
    res['dma_version'] = '%08x' % dma.rd(0x00)
    if g(0x00) != ID_VALUE:
        err('generator ID mismatch')
        print(json.dumps(res)); return 2

    flen = a.words + 4
    total_words = flen * a.frames
    if BUF_OFF + total_words * 4 + 64 > DDR_SPAN:
        err('burst does not fit the buffer'); print(json.dumps(res)); return 2
    # guard pattern over the destination, plus 16 words after it
    fill = struct.pack('<I', GUARD) * (total_words + 16)
    ddr.m[BUF_OFF:BUF_OFF + len(fill)] = fill

    def arm(dest_off, nbytes):
        irq_enable()
        ddr.wr(DESC_OFF + 4, nbytes)
        ddr.wr(DESC_OFF + 8, DDR_BASE + dest_off)
        ddr.wr(DESC_OFF + 0, DESC_CFG)
        dma.wr(STREAM0, DDR_BASE + DESC_OFF)

    def drain():
        dma.wr(CLEAR, 0xF)
        irq_enable()
        n = 0
        while select.select([uio], [], [], 0.01)[0] and n < 64:
            os.read(uio, 4); irq_enable(); n += 1
        return n

    # Completion is taken from the fabric interrupt (F2M[2], PLIC 120, exposed by uio) and
    # confirmed by the core clearing the Destination Data Ready bit of the stream descriptor.
    # Observed on the board with core version 2.0.100: INTR_0_STAT reads 0x210 (DESC_RNUM=33,
    # stream) with no flag bits set, so the flag bits are recorded but not required.
    uio = os.open(find_uio('dma-controller@60010000'), os.O_RDWR)

    def irq_enable():
        # this board's UIO driver for the DMA controller has no irqcontrol (write gives ENOSYS):
        # its interrupt stays enabled, so there is nothing to re-arm
        try:
            os.write(uio, struct.pack('<I', 1))
        except OSError:
            pass

    def wait_irq(deadline):
        r, _, _ = select.select([uio], [], [], max(0.0, deadline - time.monotonic()))
        if not r:
            return None, None
        os.read(uio, 4)
        s, ext = dma.rd(STAT), dma.rd(EXT_ADDR)
        dma.wr(CLEAR, 0xF)
        return s, ext

    if g(0x1C) & 1:
        err('generator busy at start (previous run left a packet in flight; reboot to reset the fabric)')
        print(json.dumps(res)); return 2
    dma.wr(MASK, 0xF)
    res['stale_irqs'] = drain()
    gw(0x08, 0x2); time.sleep(0.001)          # snapshot
    seq0, frames0, words0, stall0 = g(0x30), g(0x20), g(0x24), g(0x28)
    res['seq_start'] = seq0

    fbits = {'none': 0, 'crc': 1, 'seq': 2, 'payload': 4}[a.fault]
    gw(0x0C, a.words); gw(0x14, 0)

    # Burst mode: one START, one DMA transaction for all frames.
    # Per-frame mode: software-paced. For every frame the descriptor is armed first and only
    # then the source is started for exactly one frame, so the packet never shows up before
    # its descriptor is ready. (Letting the source free-run with TLAST per frame and re-arming
    # after each interrupt did not work on the board: the core did not pick up the re-armed
    # descriptor once a packet was already waiting, and the stream stalled.)
    stats = []
    ntrans = a.frames if a.per_frame else 1
    chunks = []
    lat = []
    t0 = time.monotonic()
    for i in range(ntrans):
        tprev = time.monotonic()
        if a.per_frame:
            gw(0x10, 1)
            gw(0x18, fbits if i == a.fault_frame else 0)
            arm(BUF_OFF, flen * 4)
        else:
            gw(0x10, a.frames)
            gw(0x18, (a.fault_frame << 16) | fbits)
            arm(BUF_OFF, total_words * 4)
        gw(0x08, 0x1)
        s, ext = wait_irq(time.monotonic() + a.timeout)
        if s is None:
            err('timeout waiting for DMA interrupt, transaction %d' % i); break
        stats.append(s)
        if s & 0xE: err('DMA error flags %#x on transaction %d' % (s, i))
        if ddr.rd(DESC_OFF) & 0x4: err('descriptor data-ready bit still set after transaction %d' % i)
        if ((s >> 4) & 0x3F) != 33: err('DESC_RNUM %d, expected 33 (stream)' % ((s >> 4) & 0x3F))
        if ext != DDR_BASE + DESC_OFF: err('EXT_ADDR %#x' % ext)
        if a.per_frame:
            chunks.append(bytes(ddr.m[BUF_OFF:BUF_OFF + flen * 4]))
            lat.append(time.monotonic() - tprev)
    t1 = time.monotonic()
    if a.per_frame and len(lat) > 1:
        l = sorted(lat)
        res['per_frame_loop_us'] = dict(min=round(l[0] * 1e6, 1), median=round(l[len(l) // 2] * 1e6, 1),
                                        p99=round(l[min(len(l) - 1, int(len(l) * 0.99))] * 1e6, 1), max=round(l[-1] * 1e6, 1))
    while g(0x1C) & 1 and time.monotonic() < t0 + a.timeout:
        pass
    st = g(0x1C)
    if not st & 2: err('generator not done, STATUS=%#x' % st)
    res['elapsed_s'] = round(t1 - t0, 6)
    res['dma_stat'] = ['%#x' % s for s in stats]
    gw(0x08, 0x2); time.sleep(0.001)          # snapshot
    res['gen_frames'] = g(0x20) - frames0
    res['gen_words'] = g(0x24) - words0
    res['gen_stall_cycles'] = (g(0x28) - stall0) & 0xFFFFFFFF
    if res['gen_frames'] != a.frames: err('generator sent %d frames' % res['gen_frames'])

    if a.per_frame:
        tail = bytes(ddr.m[BUF_OFF + flen * 4:BUF_OFF + (flen + 16) * 4])
        raw = b''.join(chunks).ljust(total_words * 4, b'\0') + tail
    else:
        raw = bytes(ddr.m[BUF_OFF:BUF_OFF + (total_words + 16) * 4])
    w = struct.unpack('<%dI' % (total_words + 16), raw)
    if a.dump: res['dump'] = ['%08x' % x for x in w[:a.dump]]
    if any(x != GUARD for x in w[total_words:]): err('guard words after the buffer were overwritten')

    det = dict(crc=[], seq=[], payload=[], magic=[])
    exp_seq = seq0
    ts = []
    for i in range(a.frames):
        f = w[i * flen:(i + 1) * flen]
        if f[0] != (MAGIC << 16 | a.words): det['magic'].append(i); continue
        if f[1] != exp_seq: det['seq'].append(i)
        exp_seq = f[1] + 1
        if list(f[3:-1]) != [((f[1] & 0xFFFF) << 16) | k for k in range(a.words)]: det['payload'].append(i)
        if zlib.crc32(raw[i * flen * 4:(i + 1) * flen * 4 - 4]) != f[-1]: det['crc'].append(i)
        ts.append(f[2])
    res['detected'] = det
    expect = dict(crc=[], seq=[], payload=[], magic=[])
    if a.fault == 'crc': expect['crc'] = [a.fault_frame]
    if a.fault == 'seq': expect['seq'] = [a.fault_frame]
    if a.fault == 'payload': expect['payload'] = [a.fault_frame]; expect['crc'] = [a.fault_frame]
    if det != expect: err('detected %s, expected %s' % (det, expect))
    if len(ts) > 1:
        d = [(b - c) & 0xFFFFFFFF for c, b in zip(ts, ts[1:])]
        res['frame_period_cycles'] = dict(min=min(d), max=max(d), mean=round(sum(d) / len(d), 1))
        res['fabric_burst_s'] = round(((ts[-1] - ts[0]) & 0xFFFFFFFF) / 125e6, 6)
    res['bytes'] = total_words * 4
    res['pass'] = not res['errors']
    print(json.dumps(res))
    return 0 if res['pass'] else 1


if __name__ == '__main__':
    sys.exit(main())
