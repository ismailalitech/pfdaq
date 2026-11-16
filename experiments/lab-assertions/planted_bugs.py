#!/usr/bin/env python3
"""Plant one deliberate bug at a time in a scratch copy of hw/rtl/pfdaq_stream_src.v and run
`make -C hw sim-assert` on it. Prints one block per bug: the change, what the new protocol checks
reported, and what the two older detectors (the monitors in the stream testbench and
hw/tb/check_trace.py) said about the same run. The repository is not modified.

    python3 experiments/lab-assertions/planted_bugs.py [scratch-dir] [bug-name ...]
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HW = os.path.join(ROOT, 'hw')
RTL = os.path.join(HW, 'rtl', 'pfdaq_stream_src.v')

# name: (what it models, [(old text, new text), ...]); each old text must occur exactly once
BUGS = {
    'a_wready': ("w_ready is refilled only after the register was seen empty (the H02 experiment)",
        [("wire        w_ready = ~out_valid | TREADY;", "wire        w_ready = ~out_valid;")]),
    'b_emit': ("the generator ignores w_ready and overwrites a beat the sink has not taken",
        [("wire        emit       = (run == R_RUN) && w_ready && fir_ok", "wire        emit       = (run == R_RUN) && fir_ok")]),
    'c_force': ("FORCE_ABORT no longer clears the output register",
        [("                busy_a       <= 1'b0;\n                out_valid    <= 1'b0;\n                aborted      <= 1'b1;\n",
          "                busy_a       <= 1'b0;\n                aborted      <= 1'b1;\n")]),
    'd_tdest': ("the real-time destination counter wraps one destination too early",
        [("tdest_idx <= (tdest_idx == cfg_tdn) ? 2'd0 : tdest_idx + 2'd1;", "tdest_idx <= (tdest_idx == cfg_tdn - 2'd1) ? 2'd0 : tdest_idx + 2'd1;")]),
    'e_burst_tdest': ("a burst starts on destination 1 instead of 0",
        [("                        out_tdest     <= 2'd0;\n                        rt_slots", "                        out_tdest     <= 2'd1;\n                        rt_slots")]),
    'f_underrun': ("the FIR underrun flag is set when the pre-fill ends",
        [("if ((run == R_RUN) && w_ready && !fir_ok)", "if ((run == R_PREFILL) && fir_count >= 5'd12)")]),
    'g_keep': ("the 4-byte last beat carries TKEEP 07 instead of 0F",
        [("out_keep    <= two ? 8'hFF : 8'h0F;", "out_keep    <= two ? 8'hFF : 8'h07;")]),
    'h_valid_drop': ("TVALID drops on a stalled beat once in 16 stall clocks",
        [("            if (out_valid & TREADY)\n", "            if (out_valid & (TREADY | (stall_cycles[3:0] == 4'd15)))\n")]),
    'i_long_frame': ("every frame carries one payload word more than the header says",
        [("wire        last_payload = (pay_idx == cfg_payload - 16'd1);", "wire        last_payload = (pay_idx == cfg_payload);")]),
    'j_fill_nonzero': ("the zero fill of an aborted burst is not zero",
        [("            out_data   <= 64'd0;\n            out_keep   <= (remaining", "            out_data   <= 64'd1;\n            out_keep   <= (remaining")]),
    'k_fill_last': ("the zero fill of an aborted burst never raises TLAST on an even remainder",
        [("out_last   <= (remaining <= 32'd2);", "out_last   <= (remaining <= 32'd1);")]),
}


def plant(name, scratch):
    text = open(RTL).read()
    for old, new in BUGS[name][1]:
        if text.count(old) != 1:
            sys.exit('bug %s: text to replace occurs %d times: %r' % (name, text.count(old), old[:60]))
        text = text.replace(old, new)
    path = os.path.join(scratch, name + '.v')
    open(path, 'w').write(text)
    return path


def run(name, scratch):
    rtl = plant(name, scratch)
    out = os.path.join(scratch, 'out_' + name)
    subprocess.run(['make', '-C', HW, 'sim-assert', 'OUT=' + out, 'ASSERT_RTL=' + rtl],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    res = open(os.path.join(out, 'assert_results.txt')).read().splitlines()
    fired = [(m.group(1), m.group(2), int(m.group(3))) for l in res
             for m in [re.match(r'CHECK rule +(\d+) (\S+) +(\d+)$', l)] if m and int(m.group(3)) > 0]
    first = next((l for l in res if l.startswith('CHECK FAIL')), None)
    verdict = [l for l in res if l.startswith('ASSERT RESULT')][0]
    trace = open(os.path.join(out, 'sim', 'trace.txt')).read().splitlines()
    mon = sum(1 for l in trace if l.split(' ')[0] in ('PROTOCOL_ERROR', 'VALID_GAP', 'KEEP_ERROR'))
    chk = subprocess.run([sys.executable, os.path.join(HW, 'tb', 'check_trace.py'), os.path.join(out, 'sim', 'trace.txt')],
                         capture_output=True, text=True).stdout.splitlines()
    chk_res = next((l for l in chk if l.startswith('RESULT')), 'no verdict')
    chk_bad = [l.strip() for l in chk if not l.startswith('scenario') and not l.startswith('RESULT') and l.strip()]
    print('bug %s: %s' % (name, BUGS[name][0]))
    print('  new checks     : %s' % verdict)
    print('  rules that fired: %s' % (', '.join('%s %s x%d' % f for f in fired) or 'none'))
    if first:
        print('  first report   : %s' % first)
    print('  testbench monitor lines in the trace (PROTOCOL_ERROR, VALID_GAP, KEEP_ERROR): %d' % mon)
    print('  check_trace.py : %s (first report: %s)' % (chk_res, chk_bad[0] if chk_bad else '-'))
    print()


if __name__ == '__main__':
    scratch = sys.argv[1] if len(sys.argv) > 1 else '/tmp/planted-bugs'
    os.makedirs(scratch, exist_ok=True)
    for n in (sys.argv[2:] or sorted(BUGS)):
        run(n, scratch)
