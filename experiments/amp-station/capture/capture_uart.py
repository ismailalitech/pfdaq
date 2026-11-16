#!/usr/bin/env python3
"""Timestamped capture of a serial console (hart 4 UART, 115200 8N1) without any extra package.

  capture_uart.py DEVICE RAWFILE [--baud 115200] [--max-seconds N]

Appends "epoch<TAB>text" per line to RAWFILE (see stamp.py). Opens the port read-only and never writes
to it. Stops on SIGTERM or SIGINT or after --max-seconds (hard bound, default 600). Exit status 1 if the
port cannot be opened.
"""
import argparse
import os
import select
import signal
import sys
import termios
import time

BAUDS = {9600: termios.B9600, 19200: termios.B19200, 38400: termios.B38400, 57600: termios.B57600,
         115200: termios.B115200}
stop = False


def on_signal(*_):
    global stop
    stop = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("device")
    ap.add_argument("raw")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--max-seconds", type=float, default=600.0)
    a = ap.parse_args()
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    try:
        fd = os.open(a.device, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError as e:
        print("capture_uart: cannot open %s: %s" % (a.device, e), file=sys.stderr)
        return 1
    try:
        attr = termios.tcgetattr(fd)
    except termios.error:
        attr = None                               # not a terminal (a file or FIFO in the host tests): read as is
    if attr is not None:
      attr[0] = 0                                   # iflag: no translation, no flow control
      attr[1] = 0                                 # oflag
      attr[2] = termios.CS8 | termios.CREAD | termios.CLOCAL    # 8N1, receiver on, ignore modem lines
      attr[3] = 0                                 # lflag: raw
      attr[4] = attr[5] = BAUDS[a.baud]
      attr[6][termios.VMIN] = 0
      attr[6][termios.VTIME] = 0
      termios.tcsetattr(fd, termios.TCSANOW, attr)
    end = time.time() + a.max_seconds
    buf = b""
    with open(a.raw, "ab", buffering=0) as out:
        while not stop and time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.2)
            if not r:
                continue
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if not chunk:
                time.sleep(0.05)                  # end of a plain file: wait for more
                continue
            t = time.time()
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").replace("\r", "").replace("\t", "    ")
                out.write(("%.6f\t%s\n" % (t, text)).encode())
    os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
