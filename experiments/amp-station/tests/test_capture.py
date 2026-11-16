#!/usr/bin/env python3
"""Tests of the capture and recording tools with fake input only (a pseudo-terminal stands in for the
UART, files stand in for the ssh streams). No board, no serial port, no network."""
import json
import os
import pty
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CAP = os.path.join(HERE, "..", "capture")
failures = 0


def check(cond, msg):
    global failures
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond:
        failures += 1


def lines(path):
    return [l.rstrip("\n").split("\t", 1) for l in open(path, encoding="utf-8")]


with tempfile.TemporaryDirectory() as d:
    # --- stamp.py: lines from a pipe, CR removed, tab replaced, one record per line
    raw = os.path.join(d, "p0.raw")
    t_before = time.time()
    subprocess.run([sys.executable, os.path.join(CAP, "stamp.py"), raw], input=b"first\r\nsecond\twith tab\n\nlast without newline", check=True)
    r = lines(raw)
    check([x[1] for x in r] == ["first", "second    with tab", "", "last without newline"], "stamp.py keeps lines, strips CR, replaces tabs: %s" % [x[1] for x in r])
    check(all(t_before <= float(x[0]) <= time.time() for x in r), "stamp.py times are the PC's clock")

    # --- capture_uart.py on a pseudo-terminal: partial lines, CRLF, ANSI, a line split over two writes
    master, slave = pty.openpty()
    slave_name = os.ttyname(slave)
    uraw = os.path.join(d, "uart.raw")
    p = subprocess.Popen([sys.executable, os.path.join(CAP, "capture_uart.py"), slave_name, uraw, "--max-seconds", "10"])
    time.sleep(0.5)
    os.write(master, b"\r\n[pfdaq-sup] supervisor starting\r\n")
    time.sleep(0.3)
    os.write(master, b"[station] TRIP #1: BAD_")
    time.sleep(0.4)
    os.write(master, b"FRAMES (5 > limit 2) at t=6023 ms\r\n\x1b[0m[station] second\r\n")
    time.sleep(0.5)
    p.terminate()
    rc = p.wait(timeout=5)
    u = lines(uraw)
    texts = [x[1] for x in u]
    check(rc == 0, "capture_uart.py ends on SIGTERM with status 0")
    check("[station] TRIP #1: BAD_FRAMES (5 > limit 2) at t=6023 ms" in texts, "a line split over two reads is joined: %s" % texts)
    ts = [float(x[0]) for x in u]
    check(ts == sorted(ts) and len(u) >= 3, "capture_uart.py times are non-decreasing (%d lines)" % len(u))
    r = subprocess.run([sys.executable, os.path.join(CAP, "capture_uart.py"), "/dev/does-not-exist", uraw], capture_output=True, text=True)
    check(r.returncode == 1 and "cannot open" in r.stderr, "capture_uart.py reports a port it cannot open and exits 1")
    os.close(master); os.close(slave)

    # --- recording.py merge: three panes + ops, cleaning, ordering, width
    p0 = os.path.join(d, "p0m.raw"); p1 = os.path.join(d, "p1m.raw"); p2 = os.path.join(d, "p2m.raw"); ops = os.path.join(d, "ops.raw")
    long_line = "x" * 200
    open(p0, "w").write("100.500\t[  1.0] frames 1000  bad 0\n100.600\t\x1b[32mgreen\x1b[0m text\n100.700\t   \n")
    open(p1, "w").write("100.400\t[ 0.9 cpu0 acq] run 1 started\n100.650\t" + long_line + "\n")
    open(p2, "w").write("100.450\t[station] ARMED\x07\n")
    open(ops, "w").write("100.300\t0\t$ echo reset > /dev/shm/station/cmd\n100.900\t1\t$ kill -STOP 1234\n")
    out = os.path.join(d, "rec.jsonl")
    r = subprocess.run([sys.executable, os.path.join(CAP, "recording.py"), "merge", "-o", out, "--pane", "0", p0, "--pane", "1", p1, "--pane", "2", p2, "--ops", ops],
                       capture_output=True, text=True)
    check(r.returncode == 0, "recording.py merge succeeds: %s" % r.stdout.strip())
    rec = [json.loads(l) for l in open(out)]
    check([x["t"] for x in rec] == sorted(x["t"] for x in rec), "output sorted by time")
    check(rec[0]["t"] == 0.2 and rec[0]["text"].startswith("$ echo reset"), "t counted from 0.2 s before the first line; first line is the operator command (pane %d)" % rec[0]["pane"])
    check(not any("\x1b" in x["text"] or "\x07" in x["text"] for x in rec), "ANSI and control characters removed")
    check(all(x["text"].strip() for x in rec) and len(rec) == 7, "blank lines dropped (%d records)" % len(rec))
    check(max(len(x["text"]) for x in rec) <= 108 and any(x["text"].endswith("...") for x in rec), "long lines cut to 108 characters with '...'")
    check({x["pane"] for x in rec} == {0, 1, 2}, "all three panes present")
    check(any(x["pane"] == 1 and x["text"].startswith("$ kill -STOP") for x in rec), "ops lines go to the pane they name")
    r = subprocess.run([sys.executable, os.path.join(CAP, "recording.py"), "check", out], capture_output=True, text=True)
    check(r.returncode == 0, "recording.py check accepts it: %s" % r.stdout.strip())
    bad = os.path.join(d, "bad.jsonl")
    open(bad, "w").write('{"t": 2.0, "pane": 0, "text": "a"}\n{"t": 1.0, "pane": 0, "text": "b"}\n')
    r = subprocess.run([sys.executable, os.path.join(CAP, "recording.py"), "check", bad], capture_output=True, text=True)
    check(r.returncode != 0, "recording.py check rejects an unsorted file")
    # a raw file that does not exist (a stream that never started) gives an empty pane, not a crash
    r = subprocess.run([sys.executable, os.path.join(CAP, "recording.py"), "merge", "-o", out, "--pane", "0", p0, "--pane", "2", os.path.join(d, "missing.raw")], capture_output=True, text=True)
    check(r.returncode == 0, "a missing raw file is tolerated")

print("%d failure(s)" % failures)
sys.exit(1 if failures else 0)
