#!/usr/bin/env python3
"""Offline check of the demo HSS payload against the proven one. No board needed.

Checks (exit status 1 and a FAIL line for any that does not hold):
  - same header: magic, version, set name, per-hart entry point, privilege mode, flags, chunk counts
    per hart (hart 4 enters at 0x91C00000 in machine mode, skip-OpenSBI flag set)
  - the U-Boot chunk (hart 1..3) is byte-identical to the proven one and to u-boot.bin (sha256)
  - the hart 4 chunks hold exactly the loadable sections of the demo ELF (bytes, addresses);
    zero-initialised chunks match the ELF's NOBITS sections
  - the demo ELF fits the 1 MiB reserved for hart 4 at 0x91C00000, and its resource table has the
    same contents as in the proven ELF (the section moves with the code; the run-time copy goes to
    0x91D50000 from unchanged vendor code)
Header and chunk-table fields other than sizes and CRCs of the hart 4 chunks must be equal. The few
bytes of padding that the generator leaves uninitialised differ between any two runs and are ignored.
"""
import argparse
import hashlib
import struct
import sys

REGION_BASE, REGION_SIZE = 0x91C00000, 0x100000


def parse_payload(path):
    d = open(path, "rb").read()
    magic, ver, hlen = struct.unpack_from("<IIQ", d, 0)
    cto, zto = struct.unpack_from("<QQ", d, 24)
    harts, o = [], 40
    for _ in range(4):
        ep, pm, fl = struct.unpack_from("<QBB", d, o)
        nch, first, last = struct.unpack_from("<QQQ", d, o + 16)
        harts.append(dict(entry=ep, priv=pm, flags=fl, nchunks=nch, first=first, last=last,
                          name=d[o + 40:o + 296].split(b"\0")[0].decode()))
        o += 296
    setname = d[o:o + 256].split(b"\0")[0].decode()
    chunks, p = [], cto
    while True:
        owner, _, load, ex, size, crc, _ = struct.unpack_from("<IIQQQII", d, p)
        if size == 0:
            break
        chunks.append(dict(owner=owner, load=load, exec=ex, size=size, crc=crc, data=d[load:load + size]))
        p += 40
    zi, p = [], zto
    while True:
        owner, _, ex, size = struct.unpack_from("<IIQQ", d, p)
        if size == 0:
            break
        zi.append(dict(owner=owner, exec=ex, size=size))
        p += 24
    return dict(size=len(d), magic=magic, version=ver, hdrlen=hlen, harts=harts, setname=setname,
                chunks=chunks, zi=zi)


def elf_sections(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 2, "not a 64-bit ELF"
    entry = struct.unpack_from("<Q", d, 24)[0]
    shoff, = struct.unpack_from("<Q", d, 40)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 58)
    secs = []
    for i in range(shnum):
        n, ty, fl, addr, off, size = struct.unpack_from("<IIQQQQ", d, shoff + i * shentsize)
        secs.append(dict(nameoff=n, type=ty, flags=fl, addr=addr, off=off, size=size))
    stroff = secs[shstrndx]["off"]
    for s in secs:
        s["name"] = d[stroff + s["nameoff"]:d.index(b"\0", stroff + s["nameoff"])].decode()
        s["data"] = d[s["off"]:s["off"] + s["size"]] if s["type"] != 8 else None
    return entry, secs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo-payload", required=True)
    ap.add_argument("--demo-elf", required=True)
    ap.add_argument("--proven-payload", required=True)
    ap.add_argument("--proven-elf", required=True)
    ap.add_argument("--uboot", required=True)
    a = ap.parse_args()
    bad = []

    def check(ok, msg):
        print(("ok   " if ok else "FAIL ") + msg)
        if not ok:
            bad.append(msg)

    dm, pr = parse_payload(a.demo_payload), parse_payload(a.proven_payload)
    check(dm["magic"] == pr["magic"] == 0xB007C0DE, "magic 0xB007C0DE in both")
    check(dm["version"] == pr["version"], "header version equal (%d)" % dm["version"])
    check(dm["setname"] == pr["setname"], "set name equal: %r" % dm["setname"])
    for h in range(4):
        d, p = dm["harts"][h], pr["harts"][h]
        keys = ("entry", "priv", "flags", "nchunks", "first", "last", "name")
        check(all(d[k] == p[k] for k in keys),
              "hart u54_%d: entry %#x priv %d flags %#x chunks %d name %s" %
              (h + 1, d["entry"], d["priv"], d["flags"], d["nchunks"], d["name"]))
    check(dm["harts"][3]["entry"] == REGION_BASE and dm["harts"][3]["priv"] == 3 and dm["harts"][3]["flags"] & 0x40,
          "hart 4 enters at 0x91C00000 in machine mode with skip-OpenSBI")

    ub = open(a.uboot, "rb").read()
    dub = [c for c in dm["chunks"] if c["owner"] == 1]
    pub = [c for c in pr["chunks"] if c["owner"] == 1]
    check(len(dub) == len(pub) == 1 and dub[0]["data"] == pub[0]["data"] == ub and dub[0]["crc"] == pub[0]["crc"],
          "U-Boot chunk identical to the proven one and to u-boot.bin (sha256 %s)" % hashlib.sha256(ub).hexdigest()[:16])
    check((dub[0]["exec"], dub[0]["size"]) == (pub[0]["exec"], pub[0]["size"]), "U-Boot exec address and size equal")

    entry, secs = elf_sections(a.demo_elf)
    pentry, psecs = elf_sections(a.proven_elf)
    alloc = [s for s in secs if s["flags"] & 2 and s["size"]]
    progbits = [s for s in alloc if s["type"] == 1]
    nobits = [s for s in alloc if s["type"] == 8]
    h4 = [c for c in dm["chunks"] if c["owner"] == 4]
    check(len(h4) == len(progbits) and all(c["exec"] == s["addr"] and c["data"] == s["data"] for c, s in zip(h4, progbits)),
          "hart 4: %d code/data chunks hold exactly the PROGBITS sections of the demo ELF" % len(h4))
    h4zi = [z for z in dm["zi"] if z["owner"] == 4]
    check(len(h4zi) == len(nobits) and all(z["exec"] == s["addr"] and z["size"] == s["size"] for z, s in zip(h4zi, nobits)),
          "hart 4: %d zero-initialised chunks match the NOBITS sections" % len(h4zi))
    check(entry == REGION_BASE == pentry, "ELF entry point 0x91C00000, as the proven ELF")

    lo = min(s["addr"] for s in alloc)
    hi = max(s["addr"] + s["size"] for s in alloc)
    check(lo >= REGION_BASE and hi <= REGION_BASE + REGION_SIZE,
          "ELF occupies %#x..%#x (%d KiB) inside the 1 MiB at 0x91C00000" % (lo, hi, (hi - lo) // 1024))
    rt = [s for s in secs if s["name"] == ".resource_table"]
    prt = [s for s in psecs if s["name"] == ".resource_table"]
    # The table is copied at run time to 0x91D50000 by rsc_table.c (VDEV0_VRING_BASE + 0x50000, vendor
    # file, unchanged). Its place inside the ELF is only where the linker put it after the code, so it
    # moves when the image grows; what must not change is its content and that it stays in the region.
    check(len(rt) == 1 == len(prt) and rt[0]["data"] == prt[0]["data"],
          "resource table contents equal to the proven ELF (%d bytes)" % rt[0]["size"])
    check(REGION_BASE <= rt[0]["addr"] and rt[0]["addr"] + rt[0]["size"] <= REGION_BASE + REGION_SIZE,
          "resource table section at %#x (proven ELF: %#x), inside the region; run-time copy goes to 0x91D50000 as before"
          % (rt[0]["addr"], prt[0]["addr"]))
    print("demo payload %d bytes, proven payload %d bytes" % (dm["size"], pr["size"]))
    if bad:
        print("%d check(s) FAILED" % len(bad))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
