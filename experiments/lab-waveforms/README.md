# Where the waveform figures come from

The recorded trace of the simulations (`trace.txt`) only logs transferred words. The waveform figures in the labs
(H01 to H08, H11) show signals inside the design, cycle by cycle, so they were taken from value-change dumps (VCD)
of the same testbenches. Nothing in `hw/` is modified: each file here is a second top-level module that only asks
the simulator to dump signals, or a variant of the design to show what a deliberate bug does.

Needs only Icarus Verilog. Run each command from an empty scratch directory (the dumps are large).

## Stream source (labs H01 to H06)

```sh
iverilog -g2005 -o s.vvp hw/rtl/pfdaq_stream_src.v hw/tb/tb_pfdaq_stream_src.v experiments/lab-waveforms/stream_dump.v
mkdir -p sim && vvp -n s.vvp          # writes dump.vcd (about 1 MB) and sim/trace.txt
```

`sim/trace.txt` is identical to the one `make -C hw sim-stream` writes, so the dump changes nothing about the run.
`stream_dump.v` dumps five windows of simulation time (the ones the figures use): around 2300 ns (scenario 1), 4500 ns
(scenario 2), 111 600 ns (scenario 11), 141 800 ns and 172 700 ns (scenarios 13 and 14). The values in a figure are the
signal values in those windows, one point per change.

Three figures compare the design with a deliberately broken copy of `hw/rtl/pfdaq_stream_src.v`, made in a scratch
directory and compiled in place of the original:

| Figure | Change in the scratch copy |
|---|---|
| `lab-h02-valid-gap` | `wire w_ready = ~out_valid | TREADY;` becomes `wire w_ready = ~out_valid;` |
| `lab-h05-no-prefill` | `else if (fir_count >= 5'd12)` becomes `else if (fir_count >= 5'd0)` |

## Latency unit (labs H07, H08, H11)

```sh
iverilog -g2005 -o l.vvp hw/rtl/pfdaq_core.v hw/rtl/pfdaq_stream_src.v hw/rtl/pfdaq_latency.v hw/tb/tb_pfdaq_latency.v experiments/lab-waveforms/latency_dump.v
mkdir -p sim && vvp -n l.vvp          # writes lat.vcd (about 11 MB) and sim/lat_trace.txt
```

`lab-h11-planted-bug` runs `make -C hw sim-latency` on a scratch copy of `hw/` in which `lat_now` loses its `+ 1`.

## The lost sample in the clear sweep (H08)

`tb_lost.v` drives one latency channel directly: ONESHOT, then CLEAR after `G1 + 1` clocks, then an ACK `G2 + 1` clocks after the CLEAR (the H08 page counts the clocks the same way: 11 and 4 for the first run, 71 for the control).

```sh
iverilog -g2005 -o t.vvp hw/rtl/pfdaq_latency.v experiments/lab-waveforms/tb_lost.v
vvp -n t.vvp +G1=10 +G2=3             # ACK during the sweep:    ACKS 1 LAT_LAST 15 histogram total 0
vvp -n t.vvp +G1=10 +G2=70            # ACK after the sweep:     LAT_LAST 82, histogram total 1
```
