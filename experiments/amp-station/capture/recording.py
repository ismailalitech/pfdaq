#!/usr/bin/env python3
"""Turn the raw, timestamped logs of one board run into the recording the `replay` figure plays.

  recording.py merge -o OUT.jsonl [--max-width 108] --pane 0 RAW0 --pane 1 RAW1 --pane 2 RAW2 [--ops OPS]

Raw files have one record per line: "epoch<TAB>text" (stamp.py, capture_uart.py). The ops file has
"epoch<TAB>pane<TAB>text" for the commands the script itself issued (shown as "$ command" lines).
Output: JSON lines {"t": seconds, "pane": n, "text": "line"}, sorted by t, t counted from 0.2 s before
the first line of any pane. Cleaning: ANSI escape sequences and other control characters are removed,
blank lines are dropped, lines longer than --max-width are cut and end in "...".
  recording.py check FILE.jsonl      verify the format (ordering, fields, pane numbers) and print a summary
"""
import argparse
import json
import re
import sys

ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[()][A-Z0-9]|\x1b[=>]")
CTRL = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]")


def clean(text, width):
    text = ANSI.sub("", text)
    text = CTRL.sub("", text).rstrip()
    if len(text) > width:
        text = text[:width - 3] + "..."
    return text


def read_raw(path, pane, with_pane=False):
    out = []
    try:
        f = open(path, encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return out
    with f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            parts = line.split("\t", 2 if with_pane else 1)
            try:
                t = float(parts[0])
                if with_pane:
                    pane, text = int(parts[1]), parts[2]
                else:
                    text = parts[1] if len(parts) > 1 else ""
            except (ValueError, IndexError):
                print("recording: %s line %d is not a record, skipped" % (path, n), file=sys.stderr)
                continue
            out.append((t, pane, text))
    return out


def merge(panes, ops, width):
    ev = []
    for pane, path in panes:
        ev += read_raw(path, pane)
    if ops:
        ev += read_raw(ops, 0, with_pane=True)
    ev = [(t, p, clean(x, width)) for t, p, x in ev]
    ev = [e for e in ev if e[2]]
    # stable sort by time; lines of one pane keep their order
    ev.sort(key=lambda e: e[0])
    if not ev:
        return []
    t0 = ev[0][0] - 0.2
    return [{"t": round(t - t0, 3), "pane": p, "text": x} for t, p, x in ev]


def check(path):
    last, panes, n = -1.0, {}, 0
    for i, line in enumerate(open(path, encoding="utf-8"), 1):
        r = json.loads(line)
        assert set(r) == {"t", "pane", "text"}, "line %d: keys %s" % (i, sorted(r))
        assert isinstance(r["t"], (int, float)) and r["t"] >= last, "line %d: t not sorted" % i
        assert isinstance(r["pane"], int) and 0 <= r["pane"] <= 2, "line %d: pane %r" % (i, r["pane"])
        assert isinstance(r["text"], str) and r["text"] and "\n" not in r["text"], "line %d: text" % i
        last = r["t"]
        panes[r["pane"]] = panes.get(r["pane"], 0) + 1
        n += 1
    print("%s: %d lines, %.1f s, lines per pane %s" % (path, n, last, dict(sorted(panes.items()))))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("merge")
    m.add_argument("-o", required=True)
    m.add_argument("--max-width", type=int, default=108)
    m.add_argument("--pane", nargs=2, action="append", metavar=("N", "RAW"), default=[])
    m.add_argument("--ops")
    c = sub.add_parser("check")
    c.add_argument("file")
    a = ap.parse_args()
    if a.cmd == "check":
        check(a.file)
        return 0
    rec = merge([(int(n), p) for n, p in a.pane], a.ops, a.max_width)
    with open(a.o, "w", encoding="utf-8") as f:
        for r in rec:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    check(a.o)
    return 0


if __name__ == "__main__":
    sys.exit(main())
