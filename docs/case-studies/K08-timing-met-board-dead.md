---
id: K08
title: "Timing met, board dead: the missing pin constraints"
track: case-study
level: 3
summary: A script call that replaced a file list instead of adding to it.
repro: doc
status: validated
hardware: [fabric-clocks, pfdaq-core, ihc]
sources:
  - hw/libero/overlay/reference-design-v2026.04.patch:organize_tool_files
  - hw/libero/build.sh
evidence: [timing]
related: [H10, K07, B06]
---

# Timing met, board dead: the missing pin constraints

A fabric build passed every check the build flow runs, including timing in all corners. Programmed into the board, it left the boot monitor stuck at its first fabric access. The cause was one script call that replaced a list of constraint files instead of extending it. This page records how a design can be "clean" and dead at the same time, and the guard that now prevents it.

## Symptom

The build was the first of fabric v0.5.0. It finished without errors and the timing report was clean. The fabric was programmed over JTAG as usual, with the FABRIC and SNVM components only, and the board was restarted.

The boot monitor on the E51 stopped right after

```
Initializing Mi-V IHC V2
```

On a good boot it carries on from there to set up its message queues. The project's notes describe the board as stuck in a reset loop at that point. The console capture of that run was not kept, so how often it repeated and what the other consoles showed are not recorded.

## Isolation

1. **Only the fabric had changed.** The SD card and the boot flash were the ones the previous fabric had booted from.
2. **Where it stopped is consistent with a dead fabric.** The Mi-V inter-hart communication core is a fabric block (module F03 shows its lines), so the first fabric access of the boot monitor is a plausible place to stop. This is an inference from the position of the message, not a measurement.
3. **Reprogramming the previous job over JTAG recovered the board** (module B06 explains why this should be possible: the boot flash is untouched, so the board can be reprogrammed. It was done once, here, and that is the only recovery on record). So the card and the board were healthy and the new bitstream was the problem.
4. **The build script was read for what place and route had been given.** It had been given two files: the derived timing constraints and the new clock-crossing file. The I/O and floorplan constraint files that the reference design supplies were not in the list.

Step 4 is a reading of the script, not a measurement. How the unconstrained design behaved electrically was not investigated.

## Cause

**Established.** The vendor script builds the place-and-route file list early, with every I/O constraint file and the floorplan file:

```tcl
organize_tool_files -tool {PLACEROUTE} \
    -file ".../constraint/io/MPFS_DISCOVERY_KIT_BANK_SETTINGS.pdc" \
    -file ".../constraint/io/MPFS_DISCOVERY_KIT_BOARD_MISC.pdc" \
    ... \
    -file ".../constraint/fp/SW_PLL.pdc"
```

The project then added the clock-crossing file to place and route with a second call of the same command, in the in-progress build script:

```tcl
foreach tool {PLACEROUTE VERIFYTIMING} {
    organize_tool_files -tool $tool \
        -file ".../constraint/MPFS_DISCOVERY_KIT_derived_constraints.sdc" \
        -file ".../constraint/pfdaq_cdc.sdc" \
        -module {MPFS_DISCOVERY_KIT::work} -input_type {constraint}
}
```

`organize_tool_files` does not append to the tool's list: it **replaces** it. After the second call, place and route had two files and none of the pin constraints.

**Recorded in the project's notes, not as a measurement.** The consequence is summarised there as "pins unplaced, no reference clock, fabric dead, timing still reported clean". The number of unplaced pins in the broken build was not recorded.

**Not investigated.** Why the timing check passed. A plausible reading is that timing is computed from the constraints it is given, and a design with unconstrained pins and clock has nothing to violate. That is a hypothesis about the tool, not a finding.

## Fix

The call now lists everything place and route needs, not only what was added. This is the change to the vendor build script, in `hw/libero/overlay/reference-design-v2026.04.patch` (abbreviated):

```tcl
# organize_tool_files REPLACES a tool's file list, so the place-and-route
# list must repeat every I/O and floorplan PDC from above ...
organize_tool_files -tool {PLACEROUTE} \
    -file ".../MPFS_DISCOVERY_KIT_derived_constraints.sdc" \
    -file ".../pfdaq_cdc.sdc" \
    -file ".../io/MPFS_DISCOVERY_KIT_BANK_SETTINGS.pdc" \
    ... (the other six I/O files and the floorplan file SW_PLL.pdc)
```

Timing verification gets its own, shorter list: the derived constraints and the clock-crossing file.

The list is a copy of what the vendor script had built, so a later change to that script could make it stale. That is why the second part of the fix matters.

### A guard that looks at the result

A correct list is not proved by the list. The build script checks a number the tool prints when it assigns the pins (`hw/libero/build.sh`, abbreviated):

```sh
io=$(grep -a -m1 'I/O Bank and Globals Assigner identified [0-9]* fixed' "$LOG")
if [ -n "$io" ] && ! echo "$io" | grep -q ' 0 unfixed'; then
    echo "ABORT: unplaced I/Os: $io"; ...; exit 1
fi
```

A healthy build prints:

```
I/O Bank and Globals Assigner identified 64 fixed I/O macros, 0 unfixed I/O macros
```

That line is in the published build record (`timing`, which also shows timing met and the 26 % utilisation). The guard runs while the build is still going and stops it when the count of unfixed I/Os is not zero. What this build printed before the fix is not in the records, so the number of unfixed pins in the broken build is not stated here.

The first version of the guard looked for the constraint files in the log instead of the result, and it triggered too early. It was replaced by the check above.

## How to recognise it

| What you see | What it tells you |
|---|---|
| Timing is met but the board does nothing, or stops at the first fabric access | "Timing met" says nothing about pins. Check whether the I/Os were placed where the board wires them |
| A script step that "adds a file" to a tool | Look up whether it adds or replaces. In this tool flow, `organize_tool_files` replaces |
| The previous bitstream boots the same card | The card, the boot flash and the board are fine; the new bitstream is the only variable |
| The place-and-route log lacks the `Reading User PDC file` lines for the board's pin files | The pin constraints were not given to the tool |

Two habits follow. Check the result of a build step, not only its absence of errors; this case was found by a failed board test, and the guard exists so that the next one is found by the build. And keep the previous known-good job, so a bad program is a few minutes from recovery.

Since this case the build aborts unless all 64 I/Os are placed.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [Fabric clocks and resets](../system/README.md#fabric-clocks) · [pfdaq_core](../system/README.md#pfdaq-core) · [Mi-V inter-hart communication](../system/README.md#ihc) |
| **Source** | [`hw/libero/overlay/reference-design-v2026.04.patch`](../../hw/libero/overlay/reference-design-v2026.04.patch): `organize_tool_files`<br>[`hw/libero/build.sh`](../../hw/libero/build.sh) |
| **Evidence** | [Place and route result, timing met in all corners, 64 of 64 pins placed, 26 % of the fabric](../../evidence/v0.5.1/build/timing-and-utilisation.txt) (build, fabric v0.5.1, 2026-11-15) |
| **Related** | [H10 Timing closure, and the build that lost its pins](../modules/H10-timing-closure.md)<br>[K07 Three builds lost to brackets in a constraint file](K07-brackets-in-constraints.md)<br>[B06 Programming safely: components, digests and what never to touch](../modules/B06-programming-safely.md) |
| **Status** | validated |
<!-- nav:end -->
