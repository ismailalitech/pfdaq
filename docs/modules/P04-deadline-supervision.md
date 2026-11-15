---
id: P04
title: "Deadline supervision, alarm and recovery"
track: performance
level: 3
summary: Stalling Linux, killing the consumer, and watching the supervisor notice and recover.
needs: [M03]
repro: base
status: validated
hardware: [u54-4, u54-linux]
software: [supervisor, pfdaq-cap, remoteproc-rpmsg]
sources:
  - firmware/u54_4-supervisor/pfdaq_supervisor.c:PFDAQ_S_ALARM,deadline_miss,max_lag,sup_recv,seq_mismatch
  - yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c:pause_burst,pause_ms,deadline_ms,sup_xfer
  - tests/board/pfdaq_final_suite.sh:T9-after-kill
evidence: [suite-freertos, suite-freertos-raw, suite-fresh-card-raw, suite-baremetal, supervisor-console]
related: [M02, M03, M04, P02, P03, K11, K12]
---

# Deadline supervision, alarm and recovery

After this module you can explain what the supervisor on the fourth core watches, when it raises an alarm, what it does and does not do about it, and how it recovers. You can also say what two deliberate failures, a stall and a killed process, look like from its side and from Linux's.

## Before you start

- **Level 3.** You should have read M03 (the supervisor and its message protocol) and know that Linux and the supervisor talk over RPMsg (M02).
- **Tools:** the board with the shared baseline and both consoles for the board runs. Nothing else.
- **Time:** the two tests take about five seconds.

## The idea

A watchdog is a second observer with its own clock. Something that must keep happening (here: Linux reporting every verified burst) has to be reported to the watcher, and if the report stops for longer than an agreed time, the watcher raises an alarm. Its value comes from being independent: it must not depend on the thing it is watching to notice that thing's failure. That is why the supervisor runs on its own core with its own interrupt (module P02 measured how much that independence is worth).

Three decisions define a supervision scheme:

1. **What counts as alive.** Here, a status message per burst.
2. **How long a silence is allowed.** The deadline.
3. **What happens on a miss.** Here, only a state change and a message. Nothing is restarted.

## On this board

### The protocol

Three messages go from Linux to the supervisor; the supervisor answers each. The full layout is in the architecture reference; for this module only these matter.

| Message | Sent | Effect in the supervisor |
|---|---|---|
| ARM | once, before the first burst, with `deadline_ms` and `max_lag` | state becomes ARMED; counters are cleared |
| STATUS | after every verified burst, with the burst number, the next sequence number Linux expects and Linux's error count | the deadline restarts; the sequence cross-check runs |
| DISARM | once, after the last burst | state becomes IDLE; the totals are printed |

### The state machine

The deadline is the timeout of the supervisor's receive call. While ARMED it waits for `deadline_ms`; otherwise it waits without limit. This is the whole mechanism:

```c
uint32_t timeout = (state == PFDAQ_S_ARMED) ? deadline_ms : RL_BLOCK;

ret = sup_recv(rp, &remote_addr, &msg, &len, timeout);
if (ret != RL_SUCCESS)
{
    /* no message within the deadline while armed */
    if (state == PFDAQ_S_ARMED)
    {
        deadline_miss++;
        state = PFDAQ_S_ALARM;
        printf_to(UART_APP, "[pfdaq-sup] ALARM: no status within %u ms (miss #%u, t=%u ms)\r\n", ...
```

```text
IDLE --ARM--> ARMED --silence longer than the deadline--> ALARM
                ^                                           |
                +----------- STATUS (resumed) --------------+
ARMED or ALARM --ARM--> ARMED (counters reset)      any --DISARM--> IDLE
```

Properties that follow from the code:

- **One alarm per silence.** Once in ALARM the supervisor waits without a timeout again, so a long silence is counted as **one** miss, not one per elapsed deadline.
- **A late status clears the alarm.** The next STATUS returns it to ARMED and prints "status resumed at burst N, back to ARMED". The miss stays counted until the next ARM.
- **A new ARM wipes the slate**, whatever state the supervisor was in. That is how a fresh run recovers from a dead one.
- **Time is one millisecond.** Under FreeRTOS the tick is 1 ms; the bare-metal build uses the 1 MHz machine timer divided by 1,000. The bare-metal supervisor gave the same result in the stall test.

### The cross-check

On every STATUS the supervisor also takes a snapshot of the fabric's own sequence counter and compares:

```c
/* the fabric may be ahead of Linux by at most one burst, never behind */
if ((uint32_t)(fabric_seq - msg.next_seq) > max_lag)
```

`max_lag` is set by Linux at ARM to the burst length plus 16 frames (1,040 for 1,024-frame bursts, 53 for the 37-frame test). Because the subtraction is unsigned, a Linux that claims to be ahead of the fabric also fails the test: the difference wraps to a huge number. No published record shows this check firing; every supervised run in the records has zero mismatches.

## Two deliberate failures

### 1. Stalling Linux

```sh
pfdaq_cap -r -D 300 -P 5:900 -w 252 -f 1024 -n 12
```

`-D 300` sets a 300 ms deadline. `-P 5:900` makes the tool sleep for 900 ms after verifying burst 5 and before reporting it. The supervisor sees no status for 900 ms against a 300 ms limit.

In the supervisor's console record of this test:

```text
[pfdaq-sup] ARMED: deadline 300 ms, max lag 1040 frames, fabric seq 285295
[pfdaq-sup] ALARM: no status within 300 ms (miss #1, t=105082 ms)
[pfdaq-sup] status resumed at burst 5, back to ARMED
[pfdaq-sup] DISARM from ARMED: 12 status, 1 deadline misses, 0 seq mismatches, linux errors 0, latency irqs 0
```

and in the tool's JSON for the run: `deadline_miss` 1, `status_rx` 12, `seq_mismatch` 0, and no data error at all. Two details are worth reading:

- **The stall is visible from Linux's side too.** The longest START-to-interrupt time of the run is 0.94 s, against about 0.04 s in a normal run, because the tool starts the next burst before it verifies the previous one, so the sleep falls inside that burst's START-to-interrupt time.
- **The data was never at fault.** The alarm reports a late report, not a bad one. A Linux that is slow and a Linux that is wrong are different events, and the scheme keeps them apart (module P03 covers the second).

The test passes only if the miss is there: the tool's rule is that a deadline miss is **required** when it stalled longer than the deadline and **forbidden** otherwise, so every other supervised run in the suite doubles as a check that no false alarm occurs. The default deadline is 500 ms; the normal gap between status messages is one burst, about 34 ms for the counter payload (module P01).

### 2. Killing the consumer

The suite starts a long run in the background and kills it hard after two seconds, then runs a short one:

```sh
( pfdaq_cap -r -D 300 -w 252 -f 1024 -n 100000 >/dev/null 2>&1 ) &
P=$!; sleep 2; pkill -9 -f 'pfdaq_cap -r -D 300'; wait $P 2>/dev/null; sleep 1
pfdaq_cap -r -w 252 -f 1024 -n 16
```

`kill -9` gives the process no chance to send DISARM. The supervisor stays ARMED, the status messages stop, and 300 ms after the last one it raises the alarm. In the console record of the fresh-card run:

```text
[pfdaq-sup] ARMED: deadline 300 ms, max lag 1040 frames, fabric seq 2595920
[pfdaq-sup] ALARM: no status within 300 ms (miss #1, t=412557 ms)
[pfdaq-sup] ARMED: deadline 500 ms, max lag 1040 frames, fabric seq 2654288
[pfdaq-sup] DISARM from ARMED: 16 status, 0 deadline misses, 0 seq mismatches, linux errors 0, latency irqs 0
```

- **The supervisor stays in ALARM and says nothing more.** There is no repeating alarm and no action, which is the "one alarm per silence" rule above.
- **The next run recovers it** with its own ARM. Its 16 bursts pass with no miss and no data error (`T9-after-kill`).
- **The killed run had captured a lot before it died.** The fabric's sequence counter advanced from 2,595,920 to 2,654,288 between the two ARM lines: 58,368 frames, which is 57 bursts of 1,024, about what two seconds of capture at 34 ms per burst gives.

The recovery here is the supervisor's, not the system's. Nothing restarted the dead process; the supervisor noticed, reported and was ready for the next run.

## Run it

These are the commands the project ran. The board was not used to write this page.

1. The stall test: the `pfdaq_cap -r -D 300 -P 5:900 …` line above (suite row `T9-deadline`).
2. The kill test: the four lines above (suite row `T9-after-kill`).
3. Watch the supervisor's console (the third serial interface of the board's USB connector) at the same time as the tool's output on the Linux side.

## What you should see

The console lines quoted above. In the suite's summary:

```text
| T9-deadline   | PASS | 12288 frames, 12.6 MB, errors seq/pay/crc 0/0/0, faults 0, supervisor ok (12 status, 1 misses) |
| T9-after-kill | PASS | 16384 frames, 16.8 MB, errors seq/pay/crc 0/0/0, faults 0, supervisor ok (16 status, 0 misses) |
```

The bare-metal supervisor, built from the same source, gives the same stall result in its suite: `T9-deadline` passes with 12 status messages and 1 miss. The kill test is not in the bare-metal suite.

## Evidence

- **Board, fabric v0.5.1, FreeRTOS:** the two rows above in `suite-freertos` and `suite-freertos-raw`; the second run of both in the fresh-card suite. The console timestamps in `supervisor-console` belong to the fresh-card run and agree with its JSON: the stall run's last acknowledgement carries an uptime of 105,947 ms, 865 ms after the alarm at 105,082 ms, which is roughly consistent with the 600 ms left of the pause plus the six bursts that follow it at about 34 ms each.
- **Board, bare metal:** `T9-deadline` in `suite-baremetal`.
- **Not tested:** a miss caused by the supervisor itself failing, a Linux that crashes rather than stalls, and the sequence cross-check ever firing.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| Alarms in a healthy run | the deadline is shorter than a burst plus its verification; the normal gap is about 34 ms for a counter and about 120 ms for FIR, and the default is 500 ms |
| No alarm after the consumer dies | the supervisor was never ARMED (the tool did not use `-r`), or it is already in ALARM from an earlier silence |
| `supervisor did not acknowledge ARM` | the RPMsg channel is not up; after a remoteproc stop it cannot be started again (K11) |

## Trade-offs and going further

- **Detect, do not act.** The supervisor reports a miss and nothing else. Acting on it (resetting the source, raising a hardware line) would turn a diagnostic into a safety function and needs a decision about what the safe state is. The fabric's abort (module P05) is the obvious tool.
- **The deadline is a latency budget.** Tight deadlines find problems earlier and raise false alarms under load; loose ones never raise them and notice late. The test values (300 ms and 500 ms) are an order of magnitude above the normal gap, and the stall of 900 ms is a factor of 26 above it.
- **A silent supervisor is not a healthy system.** If the supervisor itself stops, nothing reports it. A watcher of the watcher is a different design; here Linux's ARM would simply fail.
- **Exercise:** set `-P 5:250` with a 300 ms deadline and predict the result before you look; then `-P 5:350`.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [M03 A real-time supervisor: deadlines and independent checks](M03-real-time-supervisor.md) |
| **How to run** | Runs on the shared baseline: fabric v0.5.1 and the pfdaq image |
| **In the system** | [U54 hart 4](../system/README.md#u54-4) · [U54 harts 1 to 3](../system/README.md#u54-linux) · [pfdaq supervisor](../system/README.md#supervisor) · [pfdaq_cap](../system/README.md#pfdaq-cap) · [remoteproc and RPMsg](../system/README.md#remoteproc-rpmsg) |
| **Source** | [`firmware/u54_4-supervisor/pfdaq_supervisor.c`](../../firmware/u54_4-supervisor/pfdaq_supervisor.c): `PFDAQ_S_ALARM`, `deadline_miss`, `max_lag`, `sup_recv`, `seq_mismatch`<br>[`yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c`](../../yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c): `pause_burst`, `pause_ms`, `deadline_ms`, `sup_xfer`<br>[`tests/board/pfdaq_final_suite.sh`](../../tests/board/pfdaq_final_suite.sh): `T9-after-kill` |
| **Evidence** | [Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[Raw output of the FreeRTOS suite, one JSON object per test](../../evidence/v0.5.1/board/suite-freertos.jsonl) (board, fabric v0.5.1, 2026-11-15)<br>[Raw output of the fresh-card suite](../../evidence/v0.5.1/board/suite-fresh-card.jsonl) (board, fabric v0.5.1, 2026-11-15)<br>[Board suite with the bare-metal supervisor on hart 4, 8 of 8](../../evidence/v0.5.1/board/suite-baremetal.md) (board, fabric v0.5.1, 2026-11-15)<br>[Supervisor console during the fresh-card suite, with the alarm and recovery messages](../../evidence/v0.5.1/board/supervisor-console.txt) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [M02 RPMsg in practice: announce, bind, ping](M02-rpmsg-in-practice.md)<br>[M03 A real-time supervisor: deadlines and independent checks](M03-real-time-supervisor.md)<br>[M04 Same core, same code: FreeRTOS against bare metal](M04-freertos-vs-bare-metal.md)<br>[P02 Interrupt latency: bare metal, RTOS task, kernel, user space](P02-interrupt-latency.md)<br>[P03 Fault injection from the fabric up](P03-fault-injection.md)<br>[K11 remoteproc cannot restart the hart](../case-studies/K11-remoteproc-restart.md)<br>[K12 One boot in sixty, FreeRTOS never started](../case-studies/K12-freertos-no-start.md) |
| **Status** | validated |
<!-- nav:end -->
