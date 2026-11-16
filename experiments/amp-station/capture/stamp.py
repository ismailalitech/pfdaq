#!/usr/bin/env python3
"""Timestamp the lines of a stream.

  stamp.py RAWFILE [--max-seconds N]        read lines from stdin, append "epoch<TAB>text" to RAWFILE

The time is the PC's clock (time.time()) when the line is complete. Text keeps what was received except
that a carriage return is removed and a tab becomes four spaces, so the raw file stays one line per
record. Ends at end of input or after --max-seconds (a hard bound, default 600).
"""
import argparse
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("--max-seconds", type=float, default=600.0)
    a = ap.parse_args()
    end = time.time() + a.max_seconds
    with open(a.raw, "ab", buffering=0) as out:
        for line in sys.stdin.buffer:
            t = time.time()
            text = line.decode("utf-8", "replace").replace("\r", "").replace("\t", "    ").rstrip("\n")
            out.write(("%.6f\t%s\n" % (t, text)).encode())
            if t > end:
                break


if __name__ == "__main__":
    main()
