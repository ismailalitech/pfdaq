/*******************************************************************************
 * pfdaq_m08.h - demo variant only (PFDAQ_AMP_M08), project code.
 * SPDX-License-Identifier: MIT
 *
 * The part of the "three ways to signal another hart" experiment that is shared between hart 4
 * and the Linux program: the layout of the shared block, and the two answering routines of hart 4
 * (the polling loop for the flag method and the interrupt hook for the interrupt method).
 * Plain C with no hardware access: the experiment's host test (experiments/amp-m08/tests) runs
 * these same routines against threads on the PC; on the board pfdaq_supervisor.c supplies the
 * clock and the interrupt handler.
 *
 * The shared block is one 4 KiB page in the NON-CACHED DDR window (physical 0xC800_0000 + 0x1E0_0000,
 * module M05: the window that /dev/udmabuf-ddr-nc0 maps non-cached on Linux and that hart 4 reaches
 * directly at the same physical address). Neither side needs cache maintenance there.
 *
 *   flag method       Linux writes req = n, hart 4 (polling) writes ack = n
 *   interrupt method  Linux writes irq_req = n, then raises the fabric interrupt of latency channel 1
 *                     (CTRL.ONESHOT at 0x40000FC0); the interrupt handler on hart 4 writes irq_ack = n
 *   RPMsg method      does not use the block
 *
 * Only Linux writes: magic, req, irq_req, stop.  Only hart 4 writes: ack, irq_ack, irq_count,
 * poll_state, poll_answers.  One writer per word, so no atomic read-modify-write is needed anywhere.
 ******************************************************************************/
#ifndef PFDAQ_M08_H
#define PFDAQ_M08_H

#include <stdint.h>

#define M08_MAGIC        0x3830464Du       /* "MF08" little endian, written by Linux */
#define M08_VERSION      1u

/* the window hart 4 accepts for the shared block: the 32 MiB non-cached window of the device tree */
#define M08_WINDOW_BASE  0xC8000000u
#define M08_WINDOW_SIZE  0x02000000u
/* where the Linux program puts the block (offset in /dev/udmabuf-ddr-nc0): below the 0x1F00000 of the
 * shared-memory test of module M05 and away from the capture buffers at the start of the window */
#define M08_BLOCK_OFFSET 0x01E00000u
#define M08_BLOCK_SIZE   0x1000u

/* poll_state values written by hart 4 */
#define M08_POLL_IDLE    0u
#define M08_POLL_RUNNING 0x504F4C4Cu       /* "POLL" */

/* message types of the demo variant (the production types are 1..6) */
#define M08_T_SETUP      0x30u             /* burst = physical address of the block; answers what hart 4 sees there */
#define M08_T_POLL       0x31u             /* next_seq = longest time to poll in ms; answered when the loop has ended */
#define M08_T_PING       0x32u             /* answered at once, before the fabric snapshot of the production path */
#define M08_POLL_MAX_MS  10000u

struct m08_shm {
    uint32_t magic;         /* Linux: M08_MAGIC */
    uint32_t version;       /* Linux: M08_VERSION */
    uint32_t req;           /* Linux: flag method request counter */
    uint32_t ack;           /* hart 4: copy of req, written when the polling loop sees it change */
    uint32_t stop;          /* Linux: non-zero ends the polling loop */
    uint32_t irq_req;       /* Linux: interrupt method request counter */
    uint32_t irq_ack;       /* hart 4: copy of irq_req, written by the interrupt handler */
    uint32_t irq_count;     /* hart 4: interrupts handled while the block was set up */
    uint32_t poll_state;    /* hart 4: M08_POLL_RUNNING while the polling loop runs */
    uint32_t poll_answers;  /* hart 4: answers given by the polling loop since SETUP */
    uint32_t pad[54];
};

_Static_assert(sizeof(struct m08_shm) == 256, "the shared block is 64 words");
_Static_assert(sizeof(struct m08_shm) <= M08_BLOCK_SIZE, "the shared block fits its page");

/* is [addr, addr + M08_BLOCK_SIZE) inside the window, and aligned to the page? Hart 4 refuses anything else,
 * so a wrong address from Linux can never make it write somewhere that is not the shared window. */
static inline int m08_block_ok(uint64_t addr)
{
    return addr >= M08_WINDOW_BASE && addr + M08_BLOCK_SIZE <= (uint64_t)M08_WINDOW_BASE + M08_WINDOW_SIZE &&
           (addr & (M08_BLOCK_SIZE - 1u)) == 0;
}

/*
 * Flag method, hart 4 side. Runs until Linux sets stop, or until max_ms has passed according to
 * now_ms(). Returns the number of requests answered. The clock is read only every 256 passes, so it
 * adds nothing to the time between a change of req and the write of ack.
 */
static inline uint32_t m08_poll(volatile struct m08_shm *s, uint32_t (*now_ms)(void), uint32_t max_ms, uint32_t *timed_out)
{
    uint32_t t0 = now_ms(), answers = 0, last = s->req, spin = 0;

    if (max_ms > M08_POLL_MAX_MS)
        max_ms = M08_POLL_MAX_MS;
    *timed_out = 0;
    s->poll_state = M08_POLL_RUNNING;
    while (!s->stop) {
        uint32_t r = s->req;
        if (r != last) {
            s->ack = r;
            last = r;
            answers++;
        }
        if (((++spin) & 0xFFu) == 0 && (uint32_t)(now_ms() - t0) >= max_ms) {
            *timed_out = 1;
            break;
        }
    }
    s->poll_state = M08_POLL_IDLE;
    s->poll_answers += answers;
    return answers;
}

/* Interrupt method, hart 4 side: called from the interrupt handler before the fabric interrupt is acknowledged. */
static inline void m08_irq_answer(volatile struct m08_shm *s)
{
    s->irq_ack = s->irq_req;
    s->irq_count = s->irq_count + 1u;
}

#endif /* PFDAQ_M08_H */
