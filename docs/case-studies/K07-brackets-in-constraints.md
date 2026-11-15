---
id: K07
title: "Three builds lost to brackets in a constraint file"
track: case-study
level: 3
summary: Register names with brackets, and a constraint language that treats them as commands.
repro: doc
status: validated
hardware: [pfdaq-stream-src, fabric-clocks]
sources:
  - hw/libero/overlay/pfdaq_cdc.sdc
  - hw/libero/build.sh
evidence: [timing, timing-without-cdc]
related: [H03, H10, K08]
---

# Three builds lost to brackets in a constraint file

The clock-domain crossings of the stream source need false-path constraints (module H03). The obvious way to name the registers involved failed in three different ways, and each failure cost most of a build, because the error appears only after synthesis. This page records what was tried and the form that works.

## Symptom

A build with the first version of the constraint file stopped in place and route with an error from the constraint checker. Three attempts at fixing the names produced three different complaints. The project's notes record the build count, "three failed builds", in the failures table of the validation plan.

The messages, as recorded when the builds failed (paths shortened):

| Attempt | Name written for a synchroniser register | Message |
|---|---|---|
| 1 | `.../u_src/*_sync_a[0]` | `SDC0061: pfdaq_cdc.sdc:17: invalid command name "0"` |
| 2 | `.../u_src/*_sync_a\[0\]` | `SDC0023: pfdaq_cdc.sdc:17: Invalid false path constraint: the -to value [get_cells {.../*_sync_a\[...` (text cut off) |
| 3 | `.../u_src/*_sync_a?0?` | `SDC0023: pfdaq_cdc.sdc:17: Invalid false path constraint: the -to value [get_cells {.../*_sync_a?0?}] ...` (text cut off) |

The tail of the attempt 2 and 3 messages was not kept, so the exact reason the checker gives is not quoted here. A fourth form, naming the cells through `get_pins`, was also tried and rejected, according to the notes; its message was not kept.

The raw build logs of the failed runs were not kept either; the table comes from the project's notes of the session. What is published is the constraint file that works and the timing result with it.

## Isolation

Each error pointed at one line of the constraint file, so the work was reading the message, changing the name, and rebuilding. Three facts came out of it.

1. **The error appears late.** The constraint checker runs when place and route starts, after synthesis and the earlier steps. A wrong name costs the whole front half of the flow, not a quick syntax check.
2. **The names are real.** The synchroniser registers are vectors in the RTL, for example `credit_sync_a <= {credit_sync_a[1:0], credit_tgl_p}`. After synthesis their cells are named with the bit index in square brackets, so the cell exists and the intent of the line was correct. Only the way of writing the name was wrong.
3. **Square brackets mean two things in these files.** In the constraint language, as in Tcl, `[ ... ]` is a command substitution, and the same characters are part of a cell name. Attempt 1 shows the first reading: Tcl tried to run a command named `0`.

## Cause

**Established.** Attempt 1 failed because the constraint file is evaluated as Tcl and `[0]` was run as a command. That is the message `invalid command name "0"`.

**Not established.** Why attempts 2 and 3 failed. A backslash escape is the usual Tcl way to write a literal bracket, and `?` is a single-character wildcard, so both should have been able to select a bit of a vector. The checker rejected both. Whether the cause is how the tool matches cell names with indices, how it processes the escape, or something about this netlist was not investigated. Because it was unknown, the fix does not depend on it.

## Fix

Stop naming bits. Name the paths by what is at their ends: a group of registers that start in one clock domain and a group that ends in the other. This is what the file does now (`hw/libero/overlay/pfdaq_cdc.sdc`; the second line is abbreviated and both are wrapped here):

```tcl
# first stage of each 2-FF synchroniser. Bracketed register names cannot be matched here,
# so the paths are named by their endpoints: only the first stage of each chain is fed from
# the other clock domain, so these select exactly the synchroniser inputs.
set_false_path -from [ get_cells {FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src/*_tgl_p*} ] \
               -to   [ get_cells {FIC_0_PERIPHERALS_0/pfdaq_core_0/u_src/*_sync_a*} ]
set_false_path -from [ get_cells {.../u_src/busy_a* .../u_src/snap_tgl_a*} ] \
               -to   [ get_cells {.../u_src/*_sync_p*} ]
```

There is no bit index anywhere. The pattern `*_sync_a*` matches every bit of every synchroniser chain in that direction, but the `-from` side limits it: the only paths from the toggle registers `*_tgl_p*` that end in a `*_sync_a*` register are the ones into the first stage of the chain. Later stages are fed by the earlier ones, in the same domain, and are not selected.

The principle is that the constraint should be true of every path the patterns select, not of the pattern's shape. It is a weaker kind of precision than naming the bit, and it needs a check that the patterns do not reach further than intended.

That check was made: the timing analysis was re-run on the placed design without this constraint file. The record says the timing constraints are then not met (the verdict line of `timing-without-cdc`), which shows the file has an effect. The architecture document records that the violating paths were exactly the intended crossings and nothing else. The extract published here keeps only the verdict line, not the list of paths, so that second statement rests on the project's own record and is not independently reproducible from the published files.

With the file in place, timing is met in all corners and all 64 I/Os are placed (record `timing`).

### A guard so it costs minutes, not a build

`hw/libero/build.sh` watches the Libero log while it builds and stops the run as soon as a constraint error appears (abbreviated):

```sh
if grep -q 'Error: SDC00' "$LOG"; then
    echo "ABORT: constraint error"; grep -m2 'Error: SDC00' "$LOG"; ...; exit 1
fi
```

This does not make the error come sooner, but it removes the wait for the rest of the flow to give up, and it prints the message instead of leaving it in a log.

## How to recognise it

| What you see | What it tells you |
|---|---|
| `invalid command name "0"` (or any digit) from a constraint file | A bracketed index was run as a Tcl command. Quote it with braces or avoid writing it |
| A rejected constraint that names a bit of a vector, whatever the escaping | Do not spend builds on quoting variants. Select by the register groups at the path ends |
| A false-path constraint that is accepted | Accepted does not mean correct. Re-run timing without it and see that only the paths you meant to exclude are reported |
| A wildcard pattern that is much shorter than the names it matches | Check what else it selects. Let the other end of the path (`-from` or `-to`) do the narrowing |

Two habits follow. Try a new constraint form on a design that builds in minutes, not on the full one. And let the build script stop at the first constraint error, so the message is seen at once.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [Stream source](../system/README.md#pfdaq-stream-src) · [Fabric clocks and resets](../system/README.md#fabric-clocks) |
| **Source** | [`hw/libero/overlay/pfdaq_cdc.sdc`](../../hw/libero/overlay/pfdaq_cdc.sdc)<br>[`hw/libero/build.sh`](../../hw/libero/build.sh) |
| **Evidence** | [Place and route result, timing met in all corners, 64 of 64 pins placed, 26 % of the fabric](../../evidence/v0.5.1/build/timing-and-utilisation.txt) (build, fabric v0.5.1, 2026-11-15)<br>[Timing analysis without the clock-crossing constraints (the tool's console log; the 40 worst violating paths it lists are the intended crossings)](../../evidence/v0.5.1/build/timing-without-cdc-constraints.txt) (build, fabric v0.5.1, 2026-11-15) |
| **Related** | [H03 Crossing clock domains: toggles, snapshots and constraints](../modules/H03-clock-domain-crossing.md)<br>H10 Timing closure, and the build that lost its pins *(planned)*<br>[K08 Timing met, board dead: the missing pin constraints](K08-timing-met-board-dead.md) |
| **Status** | validated |
<!-- nav:end -->
