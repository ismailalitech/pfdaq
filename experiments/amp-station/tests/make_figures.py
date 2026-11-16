#!/usr/bin/env python3
"""Write docs/diagrams/labs-m10.yaml. The code figures (type terminal) take their lines straight from the
source files, so they cannot drift from the code; callouts are placed by searching the lines for a text.
  make_figures.py            write the file
  make_figures.py --check    every non-command line of every terminal figure in the file must still be a
                             line of one of the source files (tabs as four spaces, trailing space ignored)
"""
import os
import re
import sys
import yaml

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT = os.path.join(ROOT, "docs", "diagrams", "labs-m10.yaml")
SRC = {
    "st": "firmware/u54_4-supervisor/pfdaq_station.h",
    "sup": "firmware/u54_4-supervisor/pfdaq_supervisor.c",
    "com": "experiments/amp-station/station_common.h",
    "fab": "experiments/amp-station/station_fabric.h",
    "stn": "experiments/amp-station/station.c",
}
cache = {}


def src(key):
    if key not in cache:
        with open(os.path.join(ROOT, SRC[key]), encoding="utf-8") as f:
            cache[key] = [l.rstrip("\n").expandtabs(4).rstrip() for l in f]
    return cache[key]


def block(key, start, n, skip_blank=True):
    """n lines of file `key` from the first line containing `start`"""
    lines = src(key)
    i = next(k for k, l in enumerate(lines) if start in l)
    out = []
    while len(out) < n:
        if lines[i].strip() or not skip_blank:
            out.append(lines[i])
        i += 1
    return out


def between(key, start, end):
    lines = src(key)
    i = next(k for k, l in enumerate(lines) if start in l)
    j = next(k for k in range(i, len(lines)) if end in lines[k])
    return [l for l in lines[i:j + 1] if l.strip()]


def fig(id_, title, alt, lines, callouts, step=3.6, width=900):
    for l in lines:
        assert len(l) <= 108, (id_, len(l), l)
    idx = lambda pat: next(k for k, l in enumerate(lines) if pat in l)
    cs = []
    for pat_a, pat_b, t, text in callouts:
        a = idx(pat_a)
        b = idx(pat_b) if pat_b else a
        assert len(text) <= 110, (id_, len(text), text)
        cs.append({"lines": [a, b] if b != a else [a], "title": t, "text": text})
    height = 58 + 20 * len(lines) + 16 + 78
    return {"id": id_, "type": "terminal", "title": title, "alt": alt, "width": width, "height": height,
            "step": step, "lines": lines, "callouts": cs}


def build():
    figs = []

    # ---- message layout ------------------------------------------------------------------------
    L = (block("st", "struct st_msg {", 4) + block("st", "struct st_ack {", 6) +
         block("st", "_Static_assert(sizeof(struct st_msg)", 1) +
         block("st", "FEATURES    x[0] window id", 2) + block("st", "SET_LIMITS  x[0] max bad", 1))
    figs.append(fig(
        "lab-m10-message-layout", "pfdaq_station.h: the two 64-byte messages",
        "The two messages between the analysis process and hart 4, as declared in pfdaq_station.h. A request has the seven "
        "words of the production message (magic, type, burst, next_seq, errors, deadline_ms, max_lag) and nine more words x[0] "
        "to x[8]. An acknowledgement has nine words as in the production reply, then the event counter, the last trip reason, "
        "the trip count and 16 characters of text. FEATURES and SET_LIMITS use the x words.", L, [
            ("struct st_msg {", "};", "Request, 16 words", "The first seven words are the production message, so ARM, STATUS and DISARM keep their meaning."),
            ("uint32_t x[9];", None, "Nine free words", "The new messages (FEATURES, SET_LIMITS, HELLO) put their fields here, by type."),
            ("struct st_ack {", "char text[16];", "Answer, 16 words", "Nine words as in the production reply, then events, last trip reason, trip count, and a text."),
            ("uint32_t events, last_reason, trips;", None, "In every answer", "The event counter and the last trip reason come back in every acknowledgement."),
            ("_Static_assert", None, "Checked at build time", "A wrong size stops the build on hart 4 and in the Linux program."),
            ("FEATURES    x[0]", "SET_LIMITS", "Window and limits", "One window: id, frames, bad, drops, peak, mean. The limits are per window, set by Linux."),
        ]))

    # ---- the trip decision ---------------------------------------------------------------------
    L = (block("st", "    c->windows++;", 3) + block("st", "if (m->x[2] > c->lim.max_bad)", 4) +
         block("st", "    if (why)\n".strip(), 2)[:0] + ["    if (why)", "        st_trip(c, why, fseq, fframes, now);"] +
         block("st", "    if (c->state == ST_S_SAFE)", 8) +
         block("st", "st_fill_ack(c, ST_T_TRIP_NOTICE", 2) +
         block("sup", "static void st_abort_source(void)", 1))
    figs.append(fig(
        "lab-m10-trip", "pfdaq_station.h and pfdaq_supervisor.c: the trip decision on hart 4",
        "The decision on hart 4 in six steps. st_features compares the four window values with the limits and builds a reason mask; "
        "if the mask is not empty it calls st_trip. st_trip does nothing when the state is already SAFE; otherwise it stops the source "
        "through abort_source, sets the state to SAFE, counts the trip, stores the reason, and sends one unsolicited notice to the "
        "Linux endpoint. On the board abort_source writes 0x10 (CTRL.ABORT) to the stream source's control register.", L, [
            ("c->windows++;", "return;", "Only while armed", "A window that arrives in SAFE is counted and ignored: one trip per fault, not one per window."),
            ("lim.max_bad", "lim.max_mean", "Four limits, one mask", "Bad frames, drops, peak and mean of the window against the limits Linux set; each violation adds a bit."),
            ("if (why)", "st_trip(c, why", "Any bit trips", "Any violated limit is enough. The same st_trip is called for a missed deadline and a sequence mismatch."),
            ("if (c->state == ST_S_SAFE)", "return;", "Once", "A second trip while SAFE does nothing: the first reason stays recorded."),
            ("abort_source", None, "Stop the source", "The first action is to stop the data, before printing or telling anyone."),
            ("c->last_reason = reasons;", None, "Remember why", "Trip count and reason go into every later acknowledgement."),
            ("st_fill_ack(c, ST_T_TRIP_NOTICE", "c->io.send", "Tell Linux", "One unsolicited message to the endpoint that spoke last: the analysis process."),
            ("static void st_abort_source", None, "On the board", "Writing 0x10 to CTRL is the graceful ABORT of module P05: the frame in progress is finished, then it stops."),
        ], step=3.8))

    # ---- ring, producer ------------------------------------------------------------------------
    L = (block("com", "static inline void *ring_reserve", 7) + block("com", "static inline void ring_commit", 6))
    figs.append(fig(
        "lab-m10-ring-producer", "station_common.h: the producer side of the ring",
        "The producer side of the lock-free ring in shared memory. ring_reserve reads its own head counter, checks against the consumer's "
        "tail with an acquire load whether a slot is free and returns the slot or NULL. ring_commit publishes the slot with a "
        "sequentially consistent store of head and wakes the consumer with a futex only if the consumer announced that it sleeps.", L, [
            ("static inline void *ring_reserve", "head = r->h->head;", "One writer per counter", "Only the producer writes head, so it can read its own counter without a barrier."),
            ("if (head - __atomic_load_n", "return NULL;", "Full means NULL", "The producer never waits: a full ring returns NULL and the frame is counted as dropped."),
            ("return r->slots", None, "Free-running counters", "The slot is the counter modulo the number of slots, so no wrap-around case is needed."),
            ("__atomic_store_n(&r->h->head", None, "Publish", "The store makes the slot visible. Every write to the slot comes before it."),
            ("__atomic_load_n(&r->h->waiting", "FUTEX_WAKE", "Wake only if asleep", "The system call is made only when the consumer said it sleeps: normally none per frame."),
        ]))

    # ---- ring, consumer ------------------------------------------------------------------------
    L = (block("com", "static inline const void *ring_peek", 7) + block("com", "static inline void ring_release", 4) +
         block("com", "static inline int ring_wait", 15))
    figs.append(fig(
        "lab-m10-ring-consumer", "station_common.h: the consumer side of the ring",
        "The consumer side. ring_peek loads head with acquire and returns the oldest unread slot or NULL. ring_release advances tail with "
        "a release store. ring_wait announces that it sleeps, checks again, and sleeps on a futex at head with a timeout; the order of "
        "announce and check is what stops a wake-up from being lost.", L, [
            ("static inline const void *ring_peek", "head, __ATOMIC_ACQUIRE", "Acquire", "The acquire load pairs with the producer's store: everything it wrote into the slot is visible."),
            ("ring_release", "ATOMIC_RELEASE", "Give the slot back", "Advancing tail frees the slot for the producer; the release keeps our reads of the slot before it."),
            ("__atomic_store_n(&r->h->waiting, 1", None, "Announce, then look", "Sleeping starts by saying so, then the ring is checked again."),
            ("if (head == r->h->tail)", "FUTEX_WAIT", "Sleep with a limit", "If still empty it sleeps on the futex with a timeout, so the caller can run its timers."),
        ], step=3.8))

    # ---- acquisition loop ----------------------------------------------------------------------
    L = (block("stn", "struct frame_slot *slot = ring_reserve(&ra);", 15) + block("fab", "memcpy(dst, buf, h->fbytes);", 3))
    figs.append(fig(
        "lab-m10-acq-loop", "station.c and station_fabric.h: the acquisition loop on cpu 0",
        "The acquisition loop. Each pass reserves a slot of ring A, then asks the source for the next frame, which is copied straight into the slot. "
        "On a frame it counts it, stores the next expected sequence number, stamps and publishes the slot; if the ring was full it counts an overrun "
        "instead. A finished run leaves the loop. The source function copies the frame out of the DMA buffer first, re-arms the descriptor second "
        "and returns one credit third.", L, [
            ("ring_reserve(&ra)", "uint32_t seq;", "Reserve first", "A slot is reserved before the frame is read, so the copy goes straight into the ring."),
            ("r = fab->next", None, "One frame", "The source copies the frame into the slot, or discards it when the ring is full (NULL)."),
            ("slot->t_ns", "ring_commit", "Stamp and publish", "Time, run number and a first-in-run flag, then the slot becomes visible to analysis."),
            ("acq_ring_overruns++", None, "Full ring", "Acquisition never waits for analysis: the frame is dropped here and counted, and analysis sees a gap."),
            ("memcpy(dst", "SRC_CREDIT", "Copy, re-arm, credit", "The order of module I07: copy the frame out, re-arm the descriptor, only then return the credit."),
        ], step=3.8))

    # ---- supervisor hook -----------------------------------------------------------------------
    L = (block("sup", "timeout = st_timeout(&stn, timeout);", 4) + block("sup", "if (stn.state == ST_S_ARMED)", 5) +
         block("sup", "64-byte station message", 4))
    figs.append(fig(
        "lab-m10-supervisor-hook", "pfdaq_supervisor.c: how the demo variant plugs into the supervisor loop",
        "Three places in the production supervisor loop, compiled only with PFDAQ_AMP_DEMO. The receive timeout becomes the station's deadline "
        "while it is armed; when the receive times out in the armed state the station's timeout handler runs (ALARM, then trip); and a message "
        "of 64 bytes with the station magic goes to st_handle, which answers it. Messages of the production size take the old path unchanged.", L, [
            ("st_timeout", "memcpy(&msg", "Deadline as timeout", "While armed, the receive call waits at most the deadline: the same mechanism as module P04."),
            ("stn.state == ST_S_ARMED", "continue;", "Silence means alarm", "A timeout in ARMED runs st_on_timeout: the ALARM line, then the trip."),
            ("64-byte station message", "continue;", "One new branch", "A 64-byte message goes to st_handle and never reaches the old code; 28-byte messages are untouched."),
        ], step=3.8))
    return figs


def flows():
    return """\
- id: lab-m10-architecture
  alt: "The four-core station. The fabric stream source and DMA deliver one frame per millisecond to the acquisition process on Linux cpu 0 (hart 1), which copies each frame into ring A in shared memory. The analysis process on cpu 1 (hart 2) verifies every frame and sends a heartbeat every 100 ms and the window features every 500 ms over RPMsg to the FreeRTOS supervisor on hart 4. If a limit, the deadline or the sequence check is violated, hart 4 writes the abort to the stream source and sends a notice back. The analysis process passes windows and events through ring B to the telemetry process on cpu 2 (hart 3), which prints the dashboard and takes operator commands, which travel back through ring C to the analysis process."
  width: 900
  height: 470
  step: 1.3
  nodes:
    src:   {x: 10,  y: 40,  w: 160, h: 70, kind: fabric, title: "Fabric source", sub: "stream + DMA, 1 kHz"}
    acq:   {x: 185, y: 40,  w: 160, h: 70, kind: hart,   title: "CPU0 acquisition", sub: "hart 1, capture"}
    ringA: {x: 360, y: 40,  w: 160, h: 70, kind: mem,    title: "Ring A", sub: "256 frames"}
    ana:   {x: 535, y: 40,  w: 160, h: 70, kind: hart,   title: "CPU1 analysis", sub: "hart 2, owns RPMsg"}
    ringB: {x: 710, y: 40,  w: 160, h: 70, kind: mem,    title: "Ring B", sub: "windows, events"}
    abort: {x: 10,  y: 190, w: 160, h: 70, kind: fabric, title: "CTRL.ABORT", sub: "stops the source"}
    h4:    {x: 185, y: 190, w: 160, h: 70, kind: hart,   title: "Hart 4 FreeRTOS", sub: "limits, deadline, trip"}
    link:  {x: 360, y: 190, w: 160, h: 70, kind: sw,     title: "RPMsg link", sub: "64-byte messages"}
    ringC: {x: 535, y: 190, w: 160, h: 70, kind: mem,    title: "Ring C", sub: "operator commands"}
    tel:   {x: 710, y: 190, w: 160, h: 70, kind: hart,   title: "CPU2 telemetry", sub: "hart 3, dashboard"}
    uart:  {x: 185, y: 330, w: 160, h: 70, kind: sw,     title: "UART console", sub: "hart 4 prints"}
    op:    {x: 710, y: 330, w: 160, h: 70, kind: start,  title: "Operator", sub: "echo reset > cmd"}
  edges:
    - {from: src.right,  to: acq.left}
    - {from: acq.right,  to: ringA.left}
    - {from: ringA.right, to: ana.left}
    - {from: ana.bottom@0.25, to: link.top@0.75, label: "status, features"}
    - {from: link.left,  to: h4.right}
    - {from: h4.left,    to: abort.right}
    - {from: abort.top,  to: src.bottom}
    - {from: ana.right,  to: ringB.left}
    - {from: ringB.bottom, to: tel.top}
    - {from: tel.left,   to: ringC.right}
    - {from: ringC.top,  to: ana.bottom@0.75}
    - {from: h4.bottom,  to: uart.top}
    - {from: op.top,     to: tel.bottom}
  steps: [src, e0, acq, e1, ringA, e2, ana, e3, link, e4, h4, e5, abort, e6, e7, ringB, e8, tel, op, e12, e9, ringC, e10, e11, uart]
  notes:
    - {x: 10,  y: 26,  text: "Data path", bold: true}
    - {x: 10,  y: 176, text: "Protection path", bold: true}
    - {x: 535, y: 26,  text: "Report path", bold: true}
    - {x: 10,  y: 430, text: "Every box is a core or a memory: the three Linux cpus are separate processes; hart 4 is the safety supervisor."}

- id: lab-m10-message-path
  alt: "One message from a Linux cpu to hart 4 and back. The station hello program, pinned to a cpu, writes a 64-byte HELLO to the rpmsg character device; the message travels through the shared rings and the Mi-V inter-hart interrupt to hart 4; the supervisor task decodes it with st_handle and prints one line on its UART; it then sends a 64-byte acknowledgement with its own tick count, which the program reads and prints with a CLOCK_MONOTONIC time stamp."
  width: 900
  height: 340
  step: 1.4
  nodes:
    cli:  {x: 15,  y: 50,  w: 190, h: 74, kind: sw,   title: "station hello", sub: "pinned to cpu N\\nsched_getcpu()"}
    chr:  {x: 240, y: 50,  w: 190, h: 74, kind: sw,   title: "/dev/rpmsgN", sub: "write 64 bytes\\nHELLO #seq, text"}
    ring: {x: 465, y: 50,  w: 190, h: 74, kind: mem,  title: "Shared rings", sub: "0x91D0_0000, 0x91D0_8000"}
    ihc:  {x: 690, y: 50,  w: 190, h: 74, kind: mss,  title: "IHC interrupt", sub: "wakes hart 4"}
    task: {x: 690, y: 200, w: 190, h: 74, kind: hart, title: "Supervisor task", sub: "st_handle(HELLO)"}
    uart: {x: 465, y: 200, w: 190, h: 74, kind: sw,   title: "UART print", sub: "[roll-call] hart 4 got\\nHELLO #1 from hart 2"}
    ack:  {x: 240, y: 200, w: 190, h: 74, kind: hart, title: "Acknowledgement", sub: "64 bytes, its own tick\\nand a text"}
    back: {x: 15,  y: 200, w: 190, h: 74, kind: sw,   title: "station hello prints", sub: "reply and hart 4 tick\\nCLOCK_MONOTONIC time"}
  edges:
    - {from: cli.right,  to: chr.left}
    - {from: chr.right,  to: ring.left}
    - {from: ring.right, to: ihc.left}
    - {from: ihc.bottom, to: task.top}
    - {from: task.left,  to: uart.right}
    - {from: uart.left,  to: ack.right}
    - {from: ack.left,   to: back.right}
  steps: [cli, e0, chr, e1, ring, e2, ihc, e3, task, e4, uart, e5, ack, e6, back]
  notes:
    - {x: 15,  y: 36,  text: "Linux side", bold: true}
    - {x: 690, y: 186, text: "Hart 4 side", bold: true}
"""


def write():
    figs = build()
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("# Figures of module M10 (four-core monitoring station).\n"
                "# Written by experiments/amp-station/tests/make_figures.py: the code figures take their lines from the sources.\n"
                "# Do not edit by hand; run `python3 experiments/amp-station/tests/make_figures.py` and then tools/gen_diagrams.py.\n\n")
        f.write(flows())
        f.write("\n")
        f.write(yaml.dump(figs, sort_keys=False, allow_unicode=True, width=1000, default_flow_style=False))
    print("wrote", OUT, "(%d code figures)" % len(figs))


def check():
    allsrc = set()
    for k in SRC:
        allsrc.update(l.strip() for l in src(k))
    bad = 0
    for d in yaml.safe_load(open(OUT, encoding="utf-8")):
        if d.get("type") != "terminal":
            continue
        for l in d["lines"]:
            if l.startswith(("$ ", "# ")):
                continue
            if l.rstrip().strip() not in allsrc:
                print("not a source line in %s: %r" % (d["id"], l))
                bad += 1
    print("figure lines checked against the sources: %d problem(s)" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    if "--check" in sys.argv:
        sys.exit(check())
    write()
    sys.exit(check())
