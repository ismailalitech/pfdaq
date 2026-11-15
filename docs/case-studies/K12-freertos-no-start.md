---
id: K12
title: "One boot in sixty, FreeRTOS never started"
track: case-study
level: 3
summary: "An unexplained failure seen once, and what was recorded about it."
needs: [B02]
repro: doc
status: validated
hardware: [u54-4, e51]
software: [hss, supervisor]
sources:
  - tests/host/amp_reboot_test.sh
evidence: [reboot-30]
related: [B02, P06]
---

# One boot in sixty, FreeRTOS never started

Once, on one boot, the supervisor on hart 4 did not start: the boot monitor said the hart was running, but it printed nothing and never announced its message channel. It has not happened again. This page records everything that was captured about it, how many boots have passed since, and what is in place to catch it better next time. The cause is unknown.

## Symptom

On the first boot after a new boot payload had been written to the SD card, the three consoles showed this:

| Console | What it showed |
|---|---|
| Boot monitor (E51) | hart 4 reported as running |
| Hart 4 (FreeRTOS) | silent: no banner, none of the messages the supervisor prints at start |
| Linux | not recorded in detail; the supervisor's RPMsg channel never appeared |

The payload had been written from the running system, over the network, directly to its partition on the SD card. This was the first boot after that write.

The captures of that boot are recorded in the project's notes; the raw console logs are not published.

## Isolation

Little could be done, because the failure did not repeat. What was done:

- The board was rebooted. The boots that followed were normal: supervisor banner on the hart 4 console, channel present, supervised capture passing.
- Boots were then counted. At the time of the first write-up there had been 13 good boots after the failed one. The notes carried the count forward as it grew: 21, 22, 28 and 29 boots in later states of the project, with the one failure in each. The count in the current design documents is "about 60".
- No fix was attempted. Without a reproduction, a change could not have been shown to be the cure.

About 57 of those boots are accounted for here:

| Kind | Count | Result |
|---|---|---|
| Warm reboots in the first AMP reboot test | 12 | 12 of 12 |
| Warm reboots on the final payload | 6 | 6 of 6 |
| Warm reboots after the latency unit was added | 6 | 6 of 6 |
| Warm reboots in the final 30-cycle test | 30 | 30 of 30 (`reboot-30`) |
| Cold boots from freshly written SD cards | 3 | all normal |
| Other boots during development | not counted | no further failure noticed |

"Normal" for the warm reboots means the three things the reboot test checks on every cycle, described under the instrumentation below. Only the 30-cycle record is published.

## What is known and what is not

**Known**

- It happened once, on one board, on a boot that began with a payload freshly written from the running system.
- At that moment the boot monitor's state for hart 4 was "running", so the monitor had loaded the image and released the hart. The failure is after that point, or in what the hart did with it.
- It has not recurred in the boots counted above, including warm reboots, cold boots and the boots of three different fabric versions.

**Not known**

- Whether the hart ran at all. A silent hart could be hung before the first print, running without its console, or running a corrupt image.
- Whether the payload on the card was complete when that boot read it. A write from the running system may not have reached the card before the reboot. The notes do not say the write was flushed, so this is a possibility and not a finding.
- Whether it depends on a first boot after a write, or is a random event with a very low rate, such as one race in start-up between Linux's remoteproc attach and the supervisor's own start.
- Whether a cold start of the hart behaves differently. Only three cold boots were counted.

Nothing here supports a rate. One failure in about 60 boots is consistent with a true rate from roughly 0.04 % to 9 % per boot (a 95 % interval for one event). Another 60 clean boots would narrow that only a little.

## Containment

There is no containment for the failure itself. What exists is detection. If the supervisor does not start, the capture runs that use the supervisor (`-r`) cannot open its channel and fail, so the problem is visible on the first run of the suite. Reboot recovers the board.

The risk it poses for conclusions is small but real: a test run that happened to hit this would show failures in the supervisor tests that are not the fault of what was being tested. That is why the reboot test below records what happened on each boot.

## Instrumentation for next time

`tests/host/amp_reboot_test.sh` reboots the board and checks, on every cycle:

```
cycle,up_s,remoteproc,sup_channel,capture_pass,sup_pass,freertos_console_bytes
```

- `remoteproc` is the state Linux reports for hart 4 (`attached` when all is well).
- `sup_channel` is the number of supervisor channels visible to Linux (1 when well).
- `capture_pass` and `sup_pass` say whether a short supervised capture passed on the data side and on the supervisor side.
- `freertos_console_bytes` is how much the hart 4 console printed during that boot, when the script is given the console log. In the published 30-cycle run every cycle shows between 501 and 647 bytes (21 of them exactly 581), so no cycle was silent. A silent hart would show 0 here, and that column is what separates "never ran" from "ran, but the channel never opened".

A cycle that did not come back at all is recorded as `TIMEOUT`. The summary line gives the channel count and the full-pass count over all cycles.

What is still missing, and should be captured the next time it happens, in this order: the boot monitor's console for that boot, the hart 4 console, Linux's `dmesg` and the contents of `/sys/class/remoteproc/remoteproc0/`, and only then a reboot.

## Why an unreproduced failure is reported

Because leaving it out would make the validation look better than it is. A system that is reported as "30 of 30 reboots" and "27 of 27 tests" should also say that, once, something did not start, and that nobody knows why. A reader who builds this on another board and sees it once should not have to wonder whether it is their mistake. If it recurs, the notes above say what to capture.

## How to recognise it

| What you see | What it tells you |
|---|---|
| The boot monitor reports all four application harts running, but the hart 4 console is empty | This case, or a hart 4 console cable problem. Check the console cable first |
| `sup_channel` is 0 and `freertos_console_bytes` is 0 in a reboot cycle | The hart did not print anything: this case |
| `sup_channel` is 0 but `freertos_console_bytes` is not 0 | The hart ran; look at remoteproc attach and the RPMsg link instead |
| Only the first boot after writing a payload is affected | Check that the write finished (`sync`) before the reboot |

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | B02 The HSS payload: starting two operating systems *(planned)* |
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [U54 hart 4](../system/README.md#u54-4) · [E51 (hart 0)](../system/README.md#e51) · [Hart Software Services](../system/README.md#hss) · [pfdaq supervisor](../system/README.md#supervisor) |
| **Source** | [`tests/host/amp_reboot_test.sh`](../../tests/host/amp_reboot_test.sh) |
| **Evidence** | [Thirty warm reboots, both systems up and a supervised capture passing each time](../../evidence/v0.5.1/board/reboot-30.csv) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | B02 The HSS payload: starting two operating systems *(planned)*<br>P06 Soak and reboot testing *(planned)* |
| **Status** | validated |
<!-- nav:end -->
