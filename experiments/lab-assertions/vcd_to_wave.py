#!/usr/bin/env python3
"""Turn a window of a VCD into the `signals:` list of a `type: wave` figure (docs/diagrams/README.md).

    vcd_to_wave.py file.vcd T0_ns LEN_ns name=vcd.reference[|kind[|bus[|label-map[|low-bits]]]] ...

A value is written at each change, as [time since T0 in ns, value]. For a bus, the value is shown in hex
(or through the label map, e.g. 0=-,2=DATA_STABLE). Prints YAML flow lines. Times are the VCD times
divided by the time unit of the file (1 ps here, so 1000 per ns).
"""
import re, sys

def parse(path):
    ids, scopes, vals, t = {}, [], {}, 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith('$scope'):
                scopes.append(line.split()[2])
            elif line.startswith('$upscope'):
                scopes.pop()
            elif line.startswith('$var'):
                p = line.split()
                ref = '.'.join(scopes + [p[4]])
                ids.setdefault(p[3], []).append(ref)
            elif line.startswith('#'):
                t = int(line[1:])
            elif line and line[0] in '01xz' and not line.startswith('$'):
                for ref in ids.get(line[1:], []):
                    vals.setdefault(ref, []).append((t, line[0]))
            elif line.startswith('b'):
                v, i = line[1:].split()
                for ref in ids.get(i, []):
                    vals.setdefault(ref, []).append((t, v))
    return vals

def main():
    path, t0, ln = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    vals = parse(path)
    for spec in sys.argv[4:]:
        parts = spec.split('|')
        name, ref = parts[0].split('=', 1)
        kind = parts[1] if len(parts) > 1 else 'fabric'
        bus = len(parts) > 2 and parts[2] == 'bus'
        lm = dict(x.split('=', 1) for x in parts[3].split(',') if x) if len(parts) > 3 else {}
        low = int(parts[4]) if len(parts) > 4 else 0       # show only the low bits of a wide bus
        out, last = [], None
        last_at = {}
        for t, v in vals[ref]:
            last_at[t] = v          # several values at one time: the last one counts
        for t, v in sorted(last_at.items()):
            ns = t // 1000 - t0
            if ns < 0 or ns >= ln:
                continue
            if bus:
                n = int(v, 2) if re.fullmatch('[01]+', v) else None
                width = len(v)
                if n is not None and low:
                    n, width = n & ((1 << low) - 1), low
                lab = lm.get(str(n), '%0*x' % ((width + 3) // 4, n) if n is not None else '-')
                item = lab
            else:
                item = int(v) if v in '01' else 0
            if item != last:
                out.append([ns, item])
                last = item
        print('    - {name: "%s", %skind: %s, values: %s}' % (name, 'bus: true, ' if bus else '', kind,
              str(out).replace("'", '"')))

main()
