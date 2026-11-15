#!/usr/bin/env python3
"""T1: repeated warm-reboot test. Reboots the board from the Linux console N times
and checks each boot reaches the login prompt. Usage: boot_cycle_test.py <linux_tty> <N> <logdir> <root_pw>"""
import os, sys, time, termios, select
dev, n, logdir, pw = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
a = termios.tcgetattr(fd)
a[0] = 0; a[1] = 0; a[3] = 0
a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
a[4] = a[5] = termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, a)
raw = open(os.path.join(logdir, 't1-console.log'), 'ab')
res = open(os.path.join(logdir, 't1-results.csv'), 'a')
def wait_for(pat, timeout):
    buf = b''; end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 1)
        if r:
            d = os.read(fd, 4096); raw.write(d); raw.flush(); buf += d
            if pat in buf: return True
    return False
def send(s): os.write(fd, s.encode())
def login():
    send('\r')
    if not wait_for(b'login:', 20): return False
    send('root\r')
    if not wait_for(b'assword:', 10): return False
    send(pw + '\r')
    return wait_for(b':~#', 15)
if not login():
    send('\r')
    if not wait_for(b':~#', 5): sys.exit('cannot get a root shell')
ok = 0
for i in range(1, n + 1):
    send('reboot\r'); t0 = time.time()
    up = wait_for(b'Linux version', 60) and wait_for(b'mpfs-disco-kit login:', 150)
    dt = time.time() - t0
    shell = up and login()
    ok += bool(shell)
    res.write(f'{i},{int(bool(up))},{int(bool(shell))},{dt:.1f}\n'); res.flush()
    if not shell: time.sleep(30)
res.write(f'# pass {ok}/{n}\n'); res.close()
