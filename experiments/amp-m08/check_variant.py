#!/usr/bin/env python3
"""Offline check that a demo ELF holds its own variant and nothing of the others.

  check_variant.py --variant m08 --elf demo.elf --production production.elf [--objdump PATH]

Looks at the console strings of the ELF files (the firmware prints them to the hart 4 console) and, for m09,
at the machine code (the CSR instructions that must be there and the ones that must never be there).
Exit status 1 and a FAIL line for anything that does not hold.
"""
import argparse
import re
import subprocess
import sys

MARKERS = {
    "m08": [b"[m08] AMP demo variant M08", b"[m08] shared block at", b"[m08] poll ended"],
    "m09": [b"[m09] AMP demo variant M09", b"[m09] pmpcfg0"],
    "station": [b"[station] AMP demo variant"],
}


def strings_of(path):
    return open(path, "rb").read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", required=True, choices=["m08", "m09"])
    ap.add_argument("--elf", required=True)
    ap.add_argument("--production", required=True)
    ap.add_argument("--objdump")
    a = ap.parse_args()
    bad = []

    def check(ok, msg):
        print(("ok   " if ok else "FAIL ") + msg)
        if not ok:
            bad.append(msg)

    demo, prod = strings_of(a.elf), strings_of(a.production)
    for v, ms in MARKERS.items():
        if v == a.variant:
            for m in ms:
                check(m in demo, "variant ELF contains %r" % m.decode())
                check(m not in prod, "production ELF does not contain %r" % m.decode())
        else:
            for m in ms:
                check(m not in demo, "variant ELF has nothing of %s: %r absent" % (v, m.decode()))

    if a.variant == "m09":
        if not a.objdump:
            check(False, "--objdump needed for m09")
        else:
            dis = subprocess.run([a.objdump, "-d", a.elf], capture_output=True, text=True).stdout
            pdis = subprocess.run([a.objdump, "-d", a.production], capture_output=True, text=True).stdout
            lines = dis.splitlines()
            m09 = []
            cur = None
            for l in lines:
                h = re.match(r"^[0-9a-f]+ <(.+)>:$", l)
                if h:
                    cur = h.group(1)
                if cur and cur.startswith(("m09_", "pfdaq_pmp_", "user_test_")):
                    m09.append(l)
            check(len(m09) > 0, "variant ELF has m09_ functions (%d instruction lines)" % len(m09))
            check(any("mret" in l for l in m09), "m09_ code contains the one mret (to user mode)")
            check(any("ecall" in l for l in m09), "m09_ code contains the ecall that ends the user-mode test")
            check(not any(re.search(r"\bwfi\b", l) for l in m09), "m09_ code has no wfi")
    print("%d check(s) FAILED" % len(bad) if bad else "variant checks passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
