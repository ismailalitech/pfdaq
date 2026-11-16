---
id: M10
title: "A four-core monitoring station: acquire, analyse, report, protect"
track: multicore
level: 4
summary: Four cores with four different jobs, exchanging frames through shared memory and messages through RPMsg, with a protection trip on the real-time hart that stops the data source.
needs: [M02, M05]
repro: full-system
status: validated
hardware: [u54-linux, u54-4, ihc, ddr-rpmsg, pfdaq-stream-src, dma, ddr-noncached]
software: [pfdaq-cap, supervisor, remoteproc-rpmsg, uio, udmabuf]
sources:
  - firmware/u54_4-supervisor/pfdaq_station.h:st_trip,st_features,st_handle,st_on_timeout
  - firmware/u54_4-supervisor/pfdaq_supervisor.c:PFDAQ_AMP_DEMO,st_abort_source
  - experiments/amp-station/station_common.h:ring_reserve,ring_commit,ring_peek,ring_wait,verify_frame
  - experiments/amp-station/station_fabric.h:hw_next,hw_slot_done,hw_arm_rt,hw_diag
  - experiments/amp-station/station.c:role_acq,role_ana,role_tel,role_hello
  - experiments/amp-station/run_demo.sh
  - experiments/amp-station/deploy_payload.sh
evidence: [m10-station-recording, m10-station-telemetry, m10-station-first-recording, m10-station-first-telemetry, m10-stall-cap-reference, m10-stall-cap-netload, m10-stall-old-loop, m10-stall-fixed-loop, m10-stall-irq-affinity, m10-suite-after-restore]
related: [M02, M03, M04, M05, P03, P04, P05, I07, B02, B05]
---

# A four-core monitoring station: acquire, analyse, report, protect

After this module you can read a small application in which all four cores of the board have a different job and depend on each other: one acquires data from the fabric, one analyses it, one reports to an operator, and the real-time core watches all of them and stops the data source when something goes wrong. You can follow one frame, one heartbeat and one trip through the code, and you can build the same structure around your own data source.

> **Status: validated on the board, two recorded runs.** The station ran on the board with the demo payload. The first run (68 s) showed everything that was designed (the roll call, normal operation, a trip on bad frames, a trip on a missed deadline, two resets and a clean shutdown) and one thing the code was not written to do: two of its four 5,000-slot runs stopped one frame short and waited two seconds. That stall was reproduced, measured, traced to the station's own capture loop and fixed (section "The end-of-run stall"); the second recorded run (64 s, the one in the figure) has none. Afterwards the proven payload was written back and the full board suite passed again, 27 of 27. Two recorded runs are not a statistics: the limits are a first guess.

## Before you start

- **Level 4.** You should know how a Linux program and the real-time hart exchange messages (module [M02](M02-rpmsg-in-practice.md)) and how they share memory (module [M05](M05-shared-memory-linux-bare-metal.md)). The real-time capture loop that this application reuses is module [I07](I07-real-time-capture.md), the deadline check is [P04](P04-deadline-supervision.md), and the abort is [P05](P05-abort.md).
- **Board:** the shared baseline (fabric v0.5.1 and the pfdaq image) with one change: the boot payload carries the demo variant of the hart 4 firmware instead of the production supervisor. The production payload stays in `images/pfdaq-v051/` and goes back with one command (see "Run it").
- **Tools:** the PC with the project's tools, an SSH session to the board and the USB serial port of the hart 4 console.

## The idea

An industrial monitoring node usually has four things to do that must not be mixed up: take the data in without ever waiting for anyone, check it, tell a human, and shut the process down when the data or the node itself goes bad. The last job is the one that must keep working when the others do not, so it runs on its own core, with its own clock, in a different operating context.

This board has exactly that split. Three cores run Linux (cpu 0, 1 and 2, which are harts 1, 2 and 3); the fourth (hart 4) runs FreeRTOS in machine mode and was started by the boot monitor, not by Linux. The station gives each core one job:

| Core | Job | Why there |
|---|---|---|
| Linux cpu 0 (hart 1) | **Acquisition**: real-time capture from the fabric into a ring in shared memory | never waits for anybody: if the ring is full it drops and counts |
| Linux cpu 1 (hart 2) | **Analysis**: checks every frame, computes the features of each window, **owns the RPMsg link** to hart 4 | the RPMsg character device allows one opener, so all traffic to hart 4 passes here |
| Linux cpu 2 (hart 3) | **Telemetry and operator**: one dashboard line per second, a CSV file, operator commands | the part a person looks at, and the slowest |
| hart 4 (FreeRTOS) | **Safety supervisor and trip actuator**: limits on the features, a deadline on the heartbeat, a sequence check against the fabric's own counter, and the stop of the source | independent of everything Linux does |

The production supervisor already had the deadline and the sequence check (module [M03](M03-real-time-supervisor.md)). The demo variant adds the feature limits, the trip, the reset and a short roll call, without changing the production build.

![The four cores of the station. The fabric stream source and DMA deliver one frame per millisecond to the acquisition process on Linux cpu 0 (hart 1), which copies each frame into ring A in shared memory. The analysis process on cpu 1 (hart 2) verifies every frame and sends a heartbeat every 100 ms and the window features every 500 ms over RPMsg to the FreeRTOS supervisor on hart 4. If a limit, the deadline or the sequence check is violated, hart 4 writes the abort to the stream source and sends a notice back. The analysis process passes windows and events through ring B to the telemetry process on cpu 2 (hart 3), which prints the dashboard and takes operator commands, which travel back through ring C to the analysis process.](../diagrams/lab-m10-architecture.svg)

1. The fabric stream source and the DMA deliver one frame per millisecond (module [I07](I07-real-time-capture.md)).
2. Acquisition on cpu 0 copies each frame out of the DMA buffer into ring A.
3. Analysis on cpu 1 takes the frames from ring A and verifies them.
4. Every 100 ms it sends a heartbeat, and every 500 ms the features of the window, to hart 4 through the RPMsg link.
5. If a limit, the deadline or the sequence check is violated, hart 4 writes the abort to the stream source (CTRL.ABORT) and sends a notice back.
6. Windows and events go through ring B to telemetry on cpu 2, which prints them.
7. The operator's commands (`reset`, `limits`, `inject`) are written to a FIFO that telemetry reads; they travel through ring C to analysis, which relays them to hart 4.
8. Hart 4 prints every decision on its own UART console.

## On this board

### Three rings and one control block

The three Linux processes share memory under `/dev/shm/station`: one control block, and three rings, each with exactly one producer and one consumer.

| Ring | Producer to consumer | Slot | Slots | Carries |
|---|---|---|---|---|
| A | acquisition to analysis | 1,040 bytes | 256 | one frame (252 payload words, 1,024 bytes) with a time stamp and a run number |
| B | analysis to telemetry | 112 bytes | 64 | a window record or an event (trip, reset, link lost) |
| C | telemetry to analysis | 16 bytes | 16 | an operator command |

A ring is a lock-free single-producer, single-consumer queue. The two counters are free-running; each side writes only its own counter, and the producer publishes a slot with one store that the consumer reads with an acquire load, so the consumer sees all of the slot. This is the producer side:

![The producer side of the lock-free ring in shared memory. ring_reserve reads its own head counter, checks against the consumer's tail with an acquire load whether a slot is free and returns the slot or NULL. ring_commit publishes the slot with a sequentially consistent store of head and wakes the consumer with a futex only if the consumer announced that it sleeps.](../diagrams/lab-m10-ring-producer.svg)

1. Only the producer writes `head`, so it reads its own counter without a barrier.
2. A full ring returns `NULL`. The producer never waits; acquisition counts the frame as an overrun.
3. The slot is the counter modulo the number of slots; the counters wrap on their own.
4. The store of `head` publishes the slot; everything written into the slot comes before it.
5. The system call (`FUTEX_WAKE`) is made only if the consumer announced that it sleeps, so normally there is none per frame.

And the consumer side, with the order that stops a wake-up from being lost:

![The consumer side. ring_peek loads head with acquire and returns the oldest unread slot or NULL. ring_release advances tail with a release store. ring_wait announces that it sleeps, checks again, and sleeps on a futex at head with a timeout; the order of announce and check is what stops a wake-up from being lost.](../diagrams/lab-m10-ring-consumer.svg)

1. The acquire load pairs with the producer's store: everything the producer wrote into the slot is visible.
2. Advancing `tail` gives the slot back; the release store keeps the consumer's reads of the slot before it.
3. Sleeping starts by saying so (`waiting = 1`), then the ring is checked again.
4. If it is still empty the consumer sleeps on the futex with a timeout, so its caller can run its timers (the heartbeat, the window).

RISC-V is a weakly ordered architecture, so these orderings matter on the board even where they would not on a PC. The unit test of the ring was therefore run twice: on the PC with two threads, and as a static RISC-V binary under the user-mode emulator `qemu-riscv64`. That second run checks the instructions and the system calls; it is not a proof of the memory ordering of the real cores.

### Acquisition: the capture loop of module I07, with a ring behind it

Acquisition reuses the project's real-time capture: four stream descriptors, one credit per re-armed descriptor, the loop copy, re-arm, credit. The register offsets and `hw_arm_rt` are copied from `run_realtime` in `pfdaq_cap.c`, and the file says so. What is new is the use of the copy (the frame is copied straight into a slot of ring A, and runs follow each other) and one change in `hw_next`: it does not count DMA interrupts to know that a frame has arrived, it reads the descriptor of the slot (see "The end-of-run stall"; the first version counted them, as `run_realtime` does).

![The acquisition loop. Each pass reserves a slot of ring A, then asks the source for the next frame, which is copied straight into the slot. On a frame it counts it, stores the next expected sequence number, stamps and publishes the slot; if the ring was full it counts an overrun instead. The source function copies the frame out of the DMA buffer first, re-arms the descriptor second and returns one credit third.](../diagrams/lab-m10-acq-loop.svg)

1. A slot is reserved before the frame is read, so the copy goes straight into the ring.
2. The source copies the frame into the slot, or discards it if the ring is full (`NULL`).
3. The slot gets a time stamp, the run number and a "first frame of a run" flag, then it is published.
4. Acquisition never waits for analysis. A full ring means the frame is dropped here and counted; analysis sees a gap in the sequence numbers.
5. The order is the one of module I07: copy the frame out, re-arm the descriptor, and only then return the credit.

A run is a fixed number of frame slots (5,000, which is five seconds at one slot per millisecond), and runs follow each other. Hart 4 compares the sequence number that Linux reports with the fabric's own counter, which keeps its value from run to run and is reset only with the board, so `start_station.sh` first runs `station init --sync-seq`, which reads the counter into the shared control block before the analysis process arms hart 4. When hart 4 stops the source, the run ends with the abort flag set; acquisition then waits for the operator's reset before it starts another. It also looks at the state that analysis last heard from hart 4, and aborts the source itself if that state is SAFE, because an abort written between two runs is cleared by the next start (the register description in `pfdaq_stream_src.v` clears the pending abort at START). Hart 4 repeats its abort on every heartbeat it receives while SAFE for the same reason.

### Analysis: verify, summarise, ask hart 4

For every frame, analysis checks the magic word, the sequence number (a jump forward is a gap, counted as drops, not as a bad frame), the FIR payload against the project's model, and the CRC-32. The CRC routine and the payload model are copied from `pfdaq_cap.c`, which was verified on the board, and an independent Python model (with `zlib` for the CRC) is compared with the C model word for word in the tests. A frame is *bad* if any check fails. After a gap, the payload check pauses until the next run, because the records do not show whether a dropped slot consumes FIR samples; the CRC still covers the frame.

Every 500 ms it closes a *window* and sends its features to hart 4: the frame count, the bad frames, the drops, and the peak and the mean of the absolute payload values. Every 100 ms it sends a heartbeat (the production `STATUS` message with the next sequence number it expects). It also reads what hart 4 sends back, including notices that arrive unasked.

### The messages

Both directions use 64-byte messages (16 little-endian words). The first seven words of a request are the production message, so `ARM`, `STATUS` and `DISARM` mean what they mean in module M02; the production supervisor still answers its own 28-byte messages exactly as before.

![The two messages between the analysis process and hart 4, as declared in pfdaq_station.h. A request has the seven words of the production message and nine more words x[0] to x[8]. An acknowledgement has nine words as in the production reply, then the event counter, the last trip reason, the trip count and 16 characters of text. FEATURES and SET_LIMITS use the x words.](../diagrams/lab-m10-message-layout.svg)

1. A request: seven words as in the production message, then nine free words.
2. The new messages put their fields in `x[]`, by type.
3. An acknowledgement: nine words as in the production reply, then the event counter, the last trip reason and the trip count, and a text.
4. The event counter and the last trip reason come back in **every** acknowledgement.
5. A wrong size stops the build, on hart 4 and in the Linux program, because both include the same header.
6. A window: id, frames, bad frames, drops, peak and mean. The limits are per window and set by Linux.

| Type | Number | Direction | Fields |
|---|---|---|---|
| `ARM` | 1 | Linux to hart 4 | `deadline_ms`, `max_lag`; state becomes ARMED |
| `STATUS` | 2 | Linux to hart 4 | `next_seq`: the heartbeat, checked against the fabric's own counter |
| `DISARM` | 3 | Linux to hart 4 | state becomes IDLE, hart 4 prints a summary |
| `QUERY` | 4 | Linux to hart 4 | none; the acknowledgement is the answer |
| `HELLO` | 16 | Linux to hart 4 | sequence number, Linux cpu, hart, time in microseconds, 16 characters |
| `LISTEN` | 17 | Linux to hart 4 | none: asks hart 4 to speak if all three Linux harts said hello |
| `FEATURES` | 18 | Linux to hart 4 | window id, frames, bad, drops, peak, mean, window length |
| `SET_LIMITS` | 19 | Linux to hart 4 | max bad, max drops, max peak, max mean (per window; `0xFFFFFFFF` = none) |
| `RESET` | 20 | Linux to hart 4 | clears a trip and re-arms |
| `TRIP_NOTICE` | 21 | hart 4 to Linux, unasked | the acknowledgement layout with the state SAFE and the reason |
| `SPEAK` | 22 | hart 4 to Linux, unasked | 16 characters of text |

The trip reason is a bit mask: `DEADLINE` 1, `SEQUENCE` 2, `BAD_FRAMES` 4, `DROPS` 8, `PEAK` 16, `MEAN` 32. The states are `IDLE`, `ARMED`, `ALARM` and `SAFE`.

### Hart 4: the decision

The protection logic is one header, `pfdaq_station.h`. It has no hardware access: what it needs from the board (print a line, stop the source, send a message, wait) comes in through a small table of four functions. On the board the table writes to the UART, to the abort bit of the stream source and to the RPMsg endpoint. On the PC the same file runs behind a socket, which is how it was tested.

![The decision on hart 4 in steps. st_features compares the four window values with the limits and builds a reason mask; if the mask is not empty it calls st_trip. st_trip does nothing when the state is already SAFE; otherwise it stops the source through abort_source, sets the state to SAFE, counts the trip, stores the reason, and sends one unsolicited notice to the Linux endpoint. On the board abort_source writes 0x10 to the control register of the stream source.](../diagrams/lab-m10-trip.svg)

1. A window that arrives while the state is not ARMED is counted and ignored: one trip per fault, not one per window.
2. Bad frames, drops, peak and mean are compared with the limits Linux set; every violation adds a bit to the reason.
3. Any bit trips. The same `st_trip` is called for a missed deadline and for a sequence mismatch.
4. A second trip while SAFE does nothing; the first reason stays recorded.
5. The first action is to stop the source, before anything is printed or sent.
6. The trip count and the reason go into every later acknowledgement.
7. One unsolicited message goes to the endpoint that talked last, which is the analysis process.
8. On the board the stop is a write of `0x10` to the control register: the graceful ABORT of module [P05](P05-abort.md). The frame in progress is finished, then the source stops. `FORCE_ABORT` is not used because it can leave the DMA waiting.

The deadline is the receive timeout of the supervisor loop, as in module P04: while ARMED it waits at most `deadline_ms` for any message, and a timeout is ALARM followed at once by a trip. The demo variant plugs into the production loop in three places:

![Three places in the production supervisor loop, compiled only with PFDAQ_AMP_DEMO. The receive timeout becomes the station's deadline while it is armed; when the receive times out in the armed state the station's timeout handler runs; and a message of 64 bytes with the station magic goes to st_handle, which answers it.](../diagrams/lab-m10-supervisor-hook.svg)

1. While armed, the receive call waits at most the deadline: the same mechanism as in module P04.
2. A timeout in ARMED runs `st_on_timeout`: the ALARM line, then the trip.
3. A 64-byte message goes to `st_handle` and never reaches the old code; 28-byte messages are untouched.

**The production build is unchanged.** `experiments/amp-station/build_firmware.sh` builds the supervisor first without the demo switch, with the compiler the proven firmware was built with, and stops unless the loadable image is byte-identical to `images/pfdaq-v051/polarfire-soc-freertos-amp-demo.elf` (it was). Only then does it build the demo.

### The roll call: how the four cores introduce themselves

The recording starts with a ten-second introduction. A small command, `station hello`, is run three times, pinned to Linux cpu 0, 1 and 2. Each time it reads its own cpu with `sched_getcpu()`, opens the same RPMsg channel as the production tools, and sends one `HELLO` with a sequence number, the cpu and hart numbers, a time stamp from `CLOCK_MONOTONIC` and 16 characters of text. Hart 4 prints one line for each and answers with its own tick count. After the third, `station listen` asks hart 4 to speak: it answers, waits half a second, and sends one message of its own, which the command prints.

![One message from a Linux cpu to hart 4 and back. The station hello program, pinned to a cpu, writes a 64-byte HELLO to the rpmsg character device; the message travels through the shared rings and the Mi-V inter-hart interrupt to hart 4; the supervisor task decodes it and prints one line on its UART; it then sends a 64-byte acknowledgement with its own tick count, which the program reads and prints with a time stamp.](../diagrams/lab-m10-message-path.svg)

1. `station hello`, pinned to cpu N, finds its cpu with `sched_getcpu()`.
2. It writes 64 bytes to `/dev/rpmsgN`.
3. The message goes through the shared rings.
4. The inter-hart interrupt wakes hart 4.
5. The supervisor task decodes the message in `st_handle`.
6. Hart 4 prints `[roll-call] hart 4 (FreeRTOS) got HELLO #1 from hart 2 (Linux cpu 1): core 2 here`.
7. It sends a 64-byte acknowledgement with its tick count.
8. The command prints the reply with a time stamp.

The message from hart 4 that nobody asked for is not quite unasked: hart 4 can only send to an address it knows, and the address of a Linux endpoint exists only while that endpoint is open. So the fourth command opens the channel first, with `LISTEN`, which hart 4 acknowledges; its own message follows half a second later, as a separate send that Linux did not request. The trip notice works the same way, with the analysis process as the long-lived endpoint.

## The scenario

The recording follows this script, about 80 seconds long. Every step is written in `run_demo.sh` with a time limit, so it always ends.

| Step | What happens | What the code is written to print (and the recording shows) |
|---|---|---|
| 1. Roll call | `station hello` on cpu 0, 1, 2, then `station listen` | three `[roll-call] ... HELLO #1 from hart h (Linux cpu c)` lines on the hart 4 console, then `core 4 speaks` on both |
| 2. Normal operation, 15 s | the station starts; capture at 1,000 frames per second | one dashboard line per second with `bad 0 drop 0`; a hart 4 line every 5 s |
| 3. Fault A | the operator types `inject crc`: the fabric corrupts the CRC of one frame in every 100-slot run | bad frames appear; at the next window end hart 4 prints `TRIP #1: BAD_FRAMES`; the source stops; the dashboard shows `src STOP` and `h4 SAFE` |
| 3. Reset | after 5 s the operator types `reset` | hart 4 prints `RESET from operator`, the source restarts, the injection is cleared |
| 4. Fault B | `kill -STOP` on the analysis process | acquisition keeps running and counts ring overruns; the dashboard shows `SILENT`; after 500 ms hart 4 prints `ALARM` and `TRIP #2: DEADLINE`; `src STOP` |
| 4. Recovery | `kill -CONT`, then `reset` | the analysis process reads the notice, flushes ring A, and the station resumes |
| 5. Shutdown | `stop_station.sh` | `DISARM` summary on hart 4, a summary line on the dashboard |

Fault A uses the fabric's own fault injection (module [P03](P03-fault-injection.md)): the `FAULT` register corrupts one frame per run, chosen by its index within the run, and it is read when the run starts. To make a stream of bad frames rather than one, the injection mode switches to runs of 100 slots with the fault at frame 10, which gives five bad frames per 500 ms window. Fault B is not a data fault at all: nothing is wrong with the stream, and only the heartbeat stops, which is what the deadline exists for.

## Parameters

Every number below is in `struct cfg` in `station.c` (or the constants it names), is printed by each process when it starts, and can be changed with an option. They were chosen from what the project measured, not tuned on this application. The recorded run is one run and no limit was changed after it; the run tripped only on the two faults it was given, and no window of that one run reached the drop limit, which does not show how often one would.

| Parameter | Value | Why |
|---|---|---|
| Slot interval | 1,000 µs (`RATE` 125,000 cycles of 8 ns) | the row of the I07 table with 4,000 slots, 0 dropped, `pass: true` |
| Frame | 252 payload words, 1,024 bytes, FIR payload | a payload mode that passed the suite with CRC fault injection detected; verification runs at about 10 MB/s on one U54 in the recorded suite, so about 0.1 ms per frame, a tenth of a core at 1 kHz |
| Descriptors | 4, credits as in I07 | the project's real-time configuration |
| Run length | 5,000 slots (normal), 100 slots (injection) | a run of seconds keeps the gaps between runs rare; the injection runs make about ten bad frames per second |
| Ring A | 256 frames, 256 ms of data | far above the 10 ms worst RPMsg round trip recorded in module M02 |
| Heartbeat | every 100 ms | five per deadline |
| Deadline on hart 4 | 500 ms | the project's default deadline (module P04) |
| Max lag | 320 frames | ring A (256) plus the descriptors in flight plus a margin |
| Window | 500 ms, 500 frames | |
| Limit: bad frames | at most 2 per window | a single stray frame does not trip; the injection gives 5 |
| Limit: drops | at most 4 per window | the 1 ms row of module I07 had none; 4 is 0.8 % |
| Limit: peak | at most 900,000,000 | the nominal peak of the model is 795,986,176 (checked by the unit test) |
| Limit: mean | at most 320,000,000 | the nominal mean of the absolute value is 278,229,455 (checked by the unit test) |
| Acknowledgement wait | 200 ms | the worst round trip in module M02 is 10 ms |

How fast a fault is noticed follows from these numbers. A data fault is seen when the window it falls in is closed: at most one window (500 ms) plus a round trip, and up to two windows if the fault starts just after a window opened. A silent analysis process is seen 500 ms after the last message. The deadline is 500 ms and not shorter because the worst round trip in the recorded runs was 10 ms and the shortest deadline in the recorded tests was 300 ms.

## Run it

The scripts reach the board as `root@pfdaq-board`; put your board's address in that name (an `/etc/hosts` line or an SSH config entry), or set `BOARD=root@<address>` in the environment. The hart 4 console is found under `/dev/serial/by-id/`; set `UART` if your port is elsewhere.

The commands below were run on the board for the two recorded runs; each script has a header that says what it will touch.

1. **On the PC, build and test** (no board involved):

   ```sh
   make -C experiments/amp-station host board
   experiments/amp-station/build_firmware.sh
   experiments/amp-station/tests/run_tests.sh
   ```

   `build_firmware.sh` ends with the offline checks of the payload: same header and set name as the proven payload, hart 4 entering at `0x91C00000` in machine mode with OpenSBI skipped, the U-Boot chunk byte-identical to the proven one, the hart 4 chunks equal to the sections of the demo ELF, and the image inside the 1 MiB reserved for hart 4. The demo image is 620 KiB; the region is 1 MiB.

2. **Put the demo payload on the board**, with the backup first (the script refuses to install without it):

   ```sh
   experiments/amp-station/deploy_payload.sh status
   experiments/amp-station/deploy_payload.sh backup
   experiments/amp-station/deploy_payload.sh install       # asks you to type a confirmation
   ssh root@pfdaq-board reboot
   ```

   The script touches only the raw payload partition `/dev/mmcblk0p2` and `/root`. It does not reboot, and it never touches the eNVM, the boot-mode settings, keys or any lock. After the reboot the hart 4 console must print `[station] AMP demo variant: roll call and station protocol (64-byte messages) enabled`.

3. **Record the run:**

   ```sh
   experiments/amp-station/run_demo.sh
   ```

   It copies the program and two scripts to `/root`, runs the scenario above, and writes the raw logs and `recording.jsonl` under `logs/m5/station/`. The recording has three panes: the Linux operator session and dashboard (cpu 2), the logs of acquisition and analysis (cpu 0 and 1), and the hart 4 console. Reboot the board before a recording: hart 4 numbers its trips from its own start (`TRIP #1`), and the script waits for those numbers.

4. **Go back to the proven payload:**

   ```sh
   experiments/amp-station/deploy_payload.sh restore       # the proven payload, checked against its SHA-256
   ssh root@pfdaq-board reboot
   ```

   If the board does not come up, write the saved or the proven payload to partition 2 of the SD card from the PC; the header of `deploy_payload.sh` says how, and why it is left to you.

## What you should see

The run below is the recording of the second run on the real board, played back at its recorded times (gaps over 1.5 s are shortened; the clock shows the recorded time). Three panes, one time line: the operator's dashboard on Linux cpu 2, the logs of acquisition (cpu 0) and analysis (cpu 1), and the UART of hart 4.

![The recorded run of the station on the board, in three terminal panes: the Linux dashboard on cpu 2, the logs of cpus 0 and 1, and the hart 4 console, with numbered callouts for the roll call, normal operation, a trip on bad frames, a trip on a missed deadline and the summary.](../diagrams/lab-m10-recording.svg)

The recording is `evidence/v0.5.1/board/m10-station-recording.jsonl`, written by `run_demo.sh` from the time-stamped logs of the three Linux sessions and the UART. One line of the figure is one line of that file. The two `run end:` lines in the middle pane are the new note of `hw_next` (see "The end-of-run stall"): in this run two of the 5,000-slot runs completed one and two frames without a DMA interrupt of their own, and the station took them anyway.

## What the recorded run showed

These are the numbers and events of the second run (64 seconds), read from the recording, the telemetry file `m10-station-telemetry.csv` (one row per 500 ms window) and the logs. The first run (68 seconds, `m10-station-first-recording`) did the same things; where it differed is said below.

- **The roll call worked.** Hart 4 printed one line for each of the three Linux cpus, then `heard all three Linux harts (1, 2, 3)`, and after `LISTEN` it sent its own message half a second later, which the listener printed.
- **Normal operation held 1,000 frames per second** with `bad 0`, `drop 0` and the nominal values of the model (peak 547M after the first window, mean 278M); hart 4 printed a window line every tenth window, about every five seconds. The first 5,000-slot run ended on time (no wait) and the next run started at once.
- **Fault A tripped once, as designed, on five bad frames.** After `inject crc` the next injection run began at the end of the current 5,000-slot run (the fault is read when a run starts); the first window that held bad frames held 487 frames and five bad ones (limit 2), and hart 4 printed `TRIP #1: BAD_FRAMES`, stopped the source (acquisition printed `run 9 ended by ABORT (hart 4 trip)`) and went to SAFE. The dashboard then showed `src STOP` and `h4 SAFE`, and the frames stopped. About six seconds later `reset` travelled from the operator through the telemetry and analysis processes to hart 4, which printed `RESET from operator`; the source restarted and 1,000 frames per second came back. The abort that ends a real-time run is therefore seen on the board here, not only in the simulation.
- **Fault B tripped on the deadline.** `kill -STOP` on the analysis process stopped the heartbeat; 500 ms after the last message hart 4 printed `ALARM`, then `TRIP #2: DEADLINE`, and acquisition stopped. While the process was frozen acquisition had filled ring A and counted 248 overruns, which is the design: the producer never waits. The dashboard showed `SILENT` and kept saying `h4 ARMED` until the process was continued, because it learns the state of hart 4 through the analysis process; the trip notice reached it about one second after `kill -CONT`. A second `reset` cleared it.
- **The summary agreed on both sides.** Linux counted 37,589 frames verified, 5 bad frames, 0 gaps and 2 trips; hart 4's `DISARM` line counted 2 trips, 1 deadline miss and 0 sequence mismatches in 94 windows.
- **The round trip to hart 4** (features sent, acknowledgement received, once per window, 94 windows): median 1.8 ms, 95th percentile 8.7 ms, maximum 15.9 ms. Every round trip above 5 ms (8 of 94) falls in a window for which hart 4 printed a line (every tenth window, and the trip), and its polled UART blocks it for about 10 ms per line.
- **The programs ran on the cpus they were pinned to** (each prints `running on cpu N` at start), and the ring overrun count stayed 0 outside fault B.
- **The station started with the fabric's sequence counter at 0 here** (the board had just been restarted). In the first run, and in later runs started without a restart, the counter was not 0; see the next section for what that did before `--sync-seq`.

Two things in the first run were not as designed. Both are explained now.

- **A 5,000-slot run did not finish by itself** (two of the four normal runs stopped at 4,999 frames and waited two seconds). That is the end-of-run stall, next section.
- **Fault A gave three bad frames in the window, not five.** The window that tripped in the first run held 300 frames: the first injection run started 0.19 s after that window had opened (telemetry rows 48 and 49: the windows close at 24.2 s and 24.7 s, the run started at 24.4 s), and the window closed after the third bad frame (at +10, +110 and +210 ms; the fourth would have come at +310 ms). The design's five per window is what a full window of injection runs holds; the window in which the injection starts holds as many as its remaining time allows, three to five, and trips if that is more than the limit of 2. In the second run the injection started 0.0 s after a window boundary and the window held five. Nothing in the code needed changing.

## The end-of-run stall

In the first run, `run 1` and `run 4` of the station stopped at 4,999 of 5,000 frames and acquisition printed `run N: no progress for 2 s, aborting the source` two seconds later. This section is what was measured to find the cause. Every number is from a run kept in `logs/m5/stall/` and the evidence files named.

**1. Is it the station or the fabric?** The project's own real-time capture (`pfdaq_cap`, module [I07](I07-real-time-capture.md)) with the same slot interval, payload and four descriptors, 30 runs of 5,000 slots one after the other: 30 passed, none timed out (`m10-stall-cap-reference`). The station's acquisition process alone, with no analysis and no link to hart 4 (the loop of the first run, with a note added that prints the registers when a run stalls), 90 s: 17 run ends, 0 stalls. The whole station for five minutes with only one idle `ssh` session open: 62 runs, 0 stalls. The whole station with the three `ssh` sessions the demo script keeps open and the roll call first: a stall in the first run, then none in 39 more runs. So the fabric can deliver 5,000 frames with 5,000 credits; something that happens next to the program decides.

**2. What the registers say when it stalls.** The note that prints on a stall (`hw_diag`) read, in all 14 stalls of one 5-minute run (`m10-stall-old-loop`):

![Recorded output of the first version of the loop on the board: the line printed by hw_diag at a stall and the line of acquisition after two seconds. Fabric frames sent 5000, processed 4999, UIO events 4999, credits 3, the last descriptor completed with configuration word 0x9 and its buffer holding the last frame of the run.](../diagrams/lab-m10-stall-diag.svg)

1. The fabric sent all 5,000 frames (`fabric sent 5000`), is done (`status 0x2`), and its drop counter did not move during the run (3,372 in all 14 stalls: the value the suite's overload tests had left).
2. The station had taken 4,999 (11 of the 14 stalls) or 4,998 (3 of 14), and the UIO event count equalled what it had taken.
3. The fabric holds 3 credits: the one for the frame that was never taken was never returned.
4. The descriptor of the missing frame reads `0x9`, the DMA's mark that the frame is in memory (module [I04](I04-dma-stream-descriptors.md)), and its buffer holds the last sequence number of the run. **The frame was delivered; only its interrupt event was missing.** (The next run's first sequence number is one higher than the last one taken, which is the `drop 1` and the gap in the first run's dashboard.)

**3. Why the loop stopped.** `hw_next` knew that frame number `processed` was ready when the UIO event count had passed `processed`, exactly as `run_realtime` does. The kernel's handler for this DMA (`drivers/uio/uio-microchip-dma.c` in the project's kernel tree) reads the interrupt status and clears bit 0, once per interrupt. If a completion's interrupt is not counted, the count stays one short for the rest of the run: every later frame is taken one slot late (a millisecond, invisible), and the last one is never taken, because nothing will raise another event. The run then ends on the program's own 2 s limit.

**4. What makes events go missing.** Not the fabric alone: with the board otherwise idle, no event went missing in 60 runs of 5,000 slots (`m10-stall-fixed-loop`, section e). With the PC sending a stream to the board's network port, 14 of 54 runs of the first loop stalled; the loop that reads descriptors (below), on the same load, found 16 events missing in 14 of 60 runs. With the full station running and the same load, every run lost events (1 to 12 per run, 201 in 48 runs of 5,000 frames, about 0.08 %). In the kernel's interrupt table (`m10-stall-irq-affinity`) the DMA interrupt, the network interrupts, the SD card, the console and the hart 4 mailbox interrupt are all counted on cpu 0, the cpu that acquisition is pinned to, so the first explanation to test was a handler on cpu 0 delayed by that traffic for more than one slot interval, which would merge two completions into one event. The test did not support it: with the DMA interrupt moved to cpu 1 (`echo 1 > /proc/irq/81/smp_affinity_list`, restored afterwards) and the same load for five minutes, 24 of 60 runs lost events (41 in all), not fewer than the 14 of 60 (16 in all) with the interrupt on cpu 0. One run of each, so the difference between them is not established either. What is established is that the events go missing under network load and that the cause is not isolated.

**5. The proven capture tool has the same loop.** Under the same network load `pfdaq_cap` real-time mode timed out in 15 of 20 runs of 5,000 slots (`m10-stall-cap-netload`). In those runs the fabric sent 4,962 to 4,999 of the 5,000 slots (1 to 38 dropped) and the tool had verified, in 14 of the 15, three frames fewer than the fabric sent. A loop that is behind returns its credits late, so with enough missed events the fabric also runs out of credits and drops. Without load the tool passed 30 of 30. That is a limit of the I07 loop that the board suite does not meet; the tool was not changed (it is part of the proven image).

**6. The fix** is in `station_fabric.h` (Linux side only; hart 4 and the payload are unchanged). `hw_slot_done` reads bit 2 of the configuration word of the next slot's descriptor, `0xD` while armed and `0x9` when the DMA has written the frame, and `hw_next` takes a frame whenever that bit is clear. The UIO device is now only the way to sleep: if the slot is not done, the loop waits at most 2 ms for an event and looks again. At the end of a run the loop prints how many events it counted, only if there were fewer than frames (the `run end:` lines). `start_station.sh` also reads the fabric's sequence counter before the analysis process arms hart 4 (`init --sync-seq`): the first soak runs of this work tripped hart 4 on `SEQUENCE` at every start after the first, because the analysis process reported sequence number 0 while the fabric counter, which only a restart of the board resets, stood at 306,014.

**7. Result of the fix** (`m10-stall-fixed-loop`): on the loaded port, acquisition alone, 24 runs in 120 s and 60 runs in 300 s: 0 stalls, 15 of 84 runs had one or two events fewer than frames and all frames were taken. Whole station with the load, 240 s, 49 runs: 0 stalls, 0 bad frames, 2 frames dropped by the fabric (limit per window 4, no trip). The second recorded run: two 5,000-slot runs took 5,000 frames on 4,999 and 4,998 events, with no wait.

What was not done: the cause of the missing events was not isolated (no trace of the interrupt line, and the one hypothesis that was tested did not hold), the hart 4 side was not touched, and the lost-event rate of the original loop under the demo's own load is two runs of four in the first recording and one in 40 in a later reproduction, so it is a property of what else the board is doing, not a constant.

## Tests that ran on the PC

| Test | What it checks | Result |
|---|---|---|
| Ring, two threads | 2,000,000 records, order, content, no loss; then a slow consumer, where stored plus dropped equals sent | passed, on x86 and as a RISC-V binary under `qemu-riscv64` |
| Messages | word offsets of both messages, encode and decode over a socket pair, message boundaries kept | passed |
| CRC and model | CRC-32 against `zlib` values; the first frames of a run against an independent Python model | passed |
| Features | a clean window; one CRC fault; a flipped payload bit; a bad magic; a gap of 3; a repeated sequence number; two runs; a flush in the middle of a run | passed |
| Hart 4 logic | the limits are inclusive; a trip happens once; the reason is a mask; sequence and deadline trips; reset; a heartbeat while SAFE repeats the abort; the roll call | passed |
| End to end | the whole scenario with the simulated source and the stand-in for hart 4; every console line fits the figure | passed |
| Scripts | `start_station.sh`, `stop_station.sh` (even with a stopped process), `run_demo.sh` and `deploy_payload.sh` (against a plain file) | passed |

The host tests relax the limits for drops and lag, because a desktop can stall a process for a second and the limits are meant for the board. The faults the tests inject are far above those limits.

## Do the same in your project

You have a data source, a Linux system on some cores, and a core that must keep working when Linux does not. Work in this order.

1. **Give every core one job and write it down.** The station's split is acquire, analyse, report, protect. A job that must never wait (acquisition) gets a core of its own and never blocks on the next stage; the job that must outlive the others (protection) runs in the other operating context.
2. **Put one channel between Linux and the real-time core, and one owner for it.** A character device with one opener forces this; it is also the right design. Everything for the real-time core goes through the one process, and what comes back is passed on.
3. **Use rings with one producer and one consumer between the Linux stages.** Copy `ring_reserve`, `ring_commit`, `ring_peek`, `ring_release` and `ring_wait` from `station_common.h`. Make the producer return `NULL` on a full ring and count what it could not store.
4. **Decide what the protection core must be told, and what it must see for itself.** Here it is told the window features and the heartbeat, and it reads the fabric's sequence counter itself, so a Linux that lies or stops cannot hide.
5. **Write the protection logic with no hardware in it.** One header, a table of four functions (print, stop the source, send, wait). Then run it on the PC against a stand-in, as `fake_hart4.c` does, and test every decision before it goes near the board.
6. **Stop the source with the gentlest stop that leaves the system usable.** Here that is the graceful abort. Say what happens to a stop that arrives between two runs, and repeat it (hart 4 repeats it on every heartbeat while SAFE; acquisition repeats it when it sees the state).
7. **Make the trip once, remember why, and report it in every answer.** The state, the count and the reason in every acknowledgement mean that no single lost message hides a trip.
8. **Flush after a trip and after a reset.** Frames captured before a trip must not reach the first window after the reset; a frame model that depends on run order must be told when a run starts.
9. **Build the demo as a variant of the firmware.** A compile-time switch, a build script that proves the production build is unchanged, and an offline check of the payload. Keep the production payload one command away.
10. **Plan the recording before the run.** Time-stamped logs of every console, one merge step into one format, and a script that waits for each event with a time limit, so it always ends.
11. **Do not let an interrupt count decide that data is there.** Use it to sleep, and read the answer from the data's own completion mark (here the descriptor); a counter that can miss an event is one short for the rest of the run, and a run ends on the one frame nobody will announce. Print the registers when your own timeout fires, so the next stall explains itself.

## What the design does not show, and what could go wrong

These are properties of the design and of what has been tested. The recorded board runs are two runs of about a minute.

- **Two runs.** The recording was made twice and the soak runs above did not record the scenario, so there is no spread of the trips or of the round trips. The limits are a first guess: a scheduling stall of a few milliseconds on cpu 0 can drop frames and trip the station; with the network port loaded the fabric dropped 2 frames in 240 s (below the limit of 4 per window), and the soak does not say how often a window would reach 4. The limits are on the command line of `start_station.sh` and are printed at start.
- **The abort of a real-time run and the supervisor next to a real-time capture** were both seen in this run (trips 1 and 2, with hart 4 pulsing its snapshot on every message while Linux drove the runs). Neither was stressed: no long run, no repeated trips, no load on the other cpus.
- **The end-of-run stall** is fixed in the station and why events go missing under network load is not known (see the section above); the proven `pfdaq_cap` loop still has it under network load.
- **Hart 4's stack is 8 KiB** and the demo adds a 160-byte print buffer and two 64-byte messages to its deepest path. The firmware ran without a fault, but no stack measurement was made.
- **Console lines cost time.** The UART is written polled at 115,200 baud, about 10 ms for a 100-character line, during which hart 4 handles nothing else. The slowest round trips of the run coincide with those lines. The demo prints a window line only every tenth window, and on a trip.
- **The dashboard shows what the analysis process knows.** A frozen analysis process leaves it showing the old state of hart 4; the hart 4 console is the truth.
- **The payload generator leaves a few bytes of padding uninitialised**, so two payloads built from the same inputs differ in about 28 bytes. The offline check compares the structure and the contents of every chunk, not the file hash.
- **Frames dropped by acquisition and the FIR model.** After a gap, the payload check pauses until the next run; with 1 ms slots and no drops this does not occur: the second run counted 0 gaps and 0 drops in the fabric (the first run counted 2 gaps, the frames lost to the stall).
- **What was changed on the board, and put back.** Only the payload partition of the SD card (backed up first, read back after each write). After the second run the proven payload was written back, read back and compared by SHA-256, the board rebooted, and the full suite passed 27 of 27 (`m10-suite-after-restore`); the payload was backed up first and restored the same way after the first run. The frequency of the stall was measured with the demo payload installed and the soak runs used `/root/station.diag` and `/root/station.fix`, the same program with and without the change. The boot flash, the boot mode and every lock were not touched.

## Evidence

- [`m10-station-recording`](../../evidence/v0.5.1/board/m10-station-recording.jsonl): the recorded run, one JSON line per console line, with the time and the pane.
- [`m10-station-telemetry`](../../evidence/v0.5.1/board/m10-station-telemetry.csv): one row per 500 ms window of the same run.
- [`m10-station-first-recording`](../../evidence/v0.5.1/board/m10-station-first-recording.jsonl) and [`m10-station-first-telemetry`](../../evidence/v0.5.1/board/m10-station-first-telemetry.csv): the first run, with the two end-of-run stalls.
- [`m10-stall-cap-reference`](../../evidence/v0.5.1/board/m10-stall-cap-reference.txt) and [`m10-stall-cap-netload`](../../evidence/v0.5.1/board/m10-stall-cap-netload.txt): `pfdaq_cap` real-time mode, 5,000 slots at 1 ms, repeated without and with network load.
- [`m10-stall-old-loop`](../../evidence/v0.5.1/board/m10-stall-old-loop.txt): the loop of the first run with the register note: the stalls and their registers.
- [`m10-stall-fixed-loop`](../../evidence/v0.5.1/board/m10-stall-fixed-loop.txt): the loop that reads descriptors, under the same loads: the `run end:` lines.
- [`m10-stall-irq-affinity`](../../evidence/v0.5.1/board/m10-stall-irq-affinity.txt): the kernel's interrupt counts, and the run with the DMA interrupt on cpu 1.
- [`m10-suite-after-restore`](../../evidence/v0.5.1/board/m10-suite-after-restore.txt): the board suite after the proven payload was put back.

The PC tests are in `experiments/amp-station/tests/`; the offline checks of the payload are the output of `experiments/amp-station/build_firmware.sh`; the raw logs of the runs are under `logs/m5/station/` and `logs/m5/stall/` in the project's working copy.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [M02 RPMsg in practice: announce, bind, ping](M02-rpmsg-in-practice.md)<br>[M05 Shared memory between Linux and a bare-metal hart](M05-shared-memory-linux-bare-metal.md) |
| **How to run** | Needs the complete integrated system |
| **In the system** | [U54 harts 1 to 3](../system/README.md#u54-linux) · [U54 hart 4](../system/README.md#u54-4) · [Mi-V inter-hart communication](../system/README.md#ihc) · [RPMsg shared memory](../system/README.md#ddr-rpmsg) · [Stream source](../system/README.md#pfdaq-stream-src) · [CoreAXI4DMAController](../system/README.md#dma) · [Non-cached window](../system/README.md#ddr-noncached) · [pfdaq_cap](../system/README.md#pfdaq-cap) · [pfdaq supervisor](../system/README.md#supervisor) · [remoteproc and RPMsg](../system/README.md#remoteproc-rpmsg) · [UIO devices](../system/README.md#uio) · [u-dma-buf](../system/README.md#udmabuf) |
| **Source** | [`firmware/u54_4-supervisor/pfdaq_station.h`](../../firmware/u54_4-supervisor/pfdaq_station.h): `st_trip`, `st_features`, `st_handle`, `st_on_timeout`<br>[`firmware/u54_4-supervisor/pfdaq_supervisor.c`](../../firmware/u54_4-supervisor/pfdaq_supervisor.c): `PFDAQ_AMP_DEMO`, `st_abort_source`<br>[`experiments/amp-station/station_common.h`](../../experiments/amp-station/station_common.h): `ring_reserve`, `ring_commit`, `ring_peek`, `ring_wait`, `verify_frame`<br>[`experiments/amp-station/station_fabric.h`](../../experiments/amp-station/station_fabric.h): `hw_next`, `hw_slot_done`, `hw_arm_rt`, `hw_diag`<br>[`experiments/amp-station/station.c`](../../experiments/amp-station/station.c): `role_acq`, `role_ana`, `role_tel`, `role_hello`<br>[`experiments/amp-station/run_demo.sh`](../../experiments/amp-station/run_demo.sh)<br>[`experiments/amp-station/deploy_payload.sh`](../../experiments/amp-station/deploy_payload.sh) |
| **Evidence** | [The four-core monitoring station on the board, second recorded run of 64 seconds, three panes with one time line (roll call, normal operation, two trips and resets, summary)](../../evidence/v0.5.1/board/m10-station-recording.jsonl) (board, fabric v0.5.1, 2026-11-16)<br>[One row per 500 ms window of the second run: frames, bad frames, drops, peak, mean, round trip to hart 4, hart 4 state](../../evidence/v0.5.1/board/m10-station-telemetry.csv) (board, fabric v0.5.1, 2026-11-16)<br>[The first recorded run of the station (68 seconds), with the loop that counted DMA interrupt events: two of four 5,000-slot runs stopped at 4,999 frames and waited 2 s](../../evidence/v0.5.1/board/m10-station-first-recording.jsonl) (board, fabric v0.5.1, 2026-11-16)<br>[One row per 500 ms window of the first run (the windows with 0 frames are the end-of-run stalls and the stopped source)](../../evidence/v0.5.1/board/m10-station-first-telemetry.csv) (board, fabric v0.5.1, 2026-11-16)<br>[pfdaq_cap real-time mode, 5,000 slots of 1,000 us, 4 descriptors, FIR payload, 30 runs one after the other on an idle board: all 30 passed, none timed out](../../evidence/v0.5.1/board/m10-stall-cap-reference.txt) (board, fabric v0.5.1, 2026-11-16)<br>[The same pfdaq_cap run repeated 20 times while the PC streamed data to the board over ssh: 15 of 20 runs timed out, 1 to 38 frames short](../../evidence/v0.5.1/board/m10-stall-cap-netload.txt) (board, fabric v0.5.1, 2026-11-16)<br>[Stalls of the first loop with the register note: the lines printed by acquisition in five conditions (idle, whole station idle session, demo observers, network load 120 s and 300 s)](../../evidence/v0.5.1/board/m10-stall-old-loop.txt) (board, fabric v0.5.1, 2026-11-16)<br>[The loop that reads descriptors under the same loads: the run end lines (frames taken, UIO events counted) of acquisition alone idle and loaded, the whole station loaded, and the second recorded run](../../evidence/v0.5.1/board/m10-stall-fixed-loop.txt) (board, fabric v0.5.1, 2026-11-16)<br>[Interrupt counts of the DMA, network and mailbox interrupts (all on cpu 0) and the 300 s run with the DMA interrupt moved to cpu 1](../../evidence/v0.5.1/board/m10-stall-irq-affinity.txt) (board, fabric v0.5.1, 2026-11-16)<br>[The board suite after the proven payload was written back (after the second run): 27 of 27 functional tests](../../evidence/v0.5.1/board/m10-suite-after-restore.txt) (board, fabric v0.5.1, 2026-11-16) |
| **Related** | [M02 RPMsg in practice: announce, bind, ping](M02-rpmsg-in-practice.md)<br>[M03 A real-time supervisor: deadlines and independent checks](M03-real-time-supervisor.md)<br>[M04 Same core, same code: FreeRTOS against bare metal](M04-freertos-vs-bare-metal.md)<br>[M05 Shared memory between Linux and a bare-metal hart](M05-shared-memory-linux-bare-metal.md)<br>[P03 Fault injection from the fabric up](P03-fault-injection.md)<br>[P04 Deadline supervision, alarm and recovery](P04-deadline-supervision.md)<br>[P05 Abort without wedging the DMA](P05-abort.md)<br>[I07 Real-time capture with four descriptors and credits](I07-real-time-capture.md)<br>[B02 The HSS payload: starting two operating systems](B02-hss-payload.md)<br>[B05 Updating a running board over SSH](B05-update-running-board.md) |
| **Status** | validated |
<!-- nav:end -->
