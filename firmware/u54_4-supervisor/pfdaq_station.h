/*******************************************************************************
 * pfdaq_station.h - demo variant only (PFDAQ_AMP_DEMO), project code.
 * SPDX-License-Identifier: MIT
 *
 * The part of the "four-core monitoring station" that runs on hart 4: the protocol with the
 * Linux analysis process, the protection logic (limits, trip, reset) and the short roll-call
 * used as an introduction. Everything here is plain C with no hardware access: the three things
 * it needs from its environment (print a line, stop the data source, send a message) come in
 * through struct st_io. On the board pfdaq_supervisor.c provides them (UART, CTRL.ABORT register
 * of the stream source, RPMsg); the experiment's host test (experiments/amp-station/tests)
 * provides stand-ins and runs this same file on the PC.
 *
 * Messages are 16 little-endian 32-bit words (64 bytes) in both directions. The first seven
 * words of a message have the layout of struct pfdaq_msg of the production supervisor, so
 * ARM, STATUS and DISARM mean the same as there. Messages of the production size (28 bytes)
 * are not handled here; the supervisor keeps handling those exactly as before.
 *
 *   Linux to hart 4 (struct st_msg)
 *     w0 magic 0x53444650   w1 type      w2 burst   w3 next_seq   w4 errors
 *     w5 deadline_ms        w6 max_lag   w7..w15 x[0..8], by type:
 *       HELLO       burst = sequence number, next_seq = Linux cpu, errors = hart number,
 *                   deadline_ms/max_lag = Linux CLOCK_MONOTONIC in us (low, high),
 *                   x[0..3] = 16 characters of text
 *       FEATURES    x[0] window id, x[1] frames, x[2] bad frames, x[3] drops, x[4] peak,
 *                   x[5] mean, x[6] window length in ms
 *       SET_LIMITS  x[0] max bad frames, x[1] max drops, x[2] max peak, x[3] max mean
 *                   (all per window; 0xFFFFFFFF = no limit)
 *       ARM         deadline_ms, max_lag as in the production supervisor
 *       STATUS      next_seq = sequence number Linux expects next (heartbeat)
 *
 *   hart 4 to Linux (struct st_ack), one per message, and unsolicited TRIP_NOTICE / SPEAK
 *     w0 magic 0x41444650   w1 type of the message answered
 *     w2 state (IDLE 0, ARMED 1, ALARM 2, SAFE 3)   w3 status_rx   w4 deadline_miss
 *     w5 seq_mismatch   w6 fabric_seq   w7 fabric_frames   w8 uptime_ms (hart 4 tick)
 *     w9 events (station messages handled)   w10 last_reason (bit mask)   w11 trips
 *     w12..w15 text[16]
 ******************************************************************************/
#ifndef PFDAQ_STATION_H
#define PFDAQ_STATION_H

#include <stdint.h>
#include <string.h>

#define ST_MSG_MAGIC    0x53444650u     /* "PFDS" */
#define ST_ACK_MAGIC    0x41444650u     /* "PFDA" */

enum st_type {
    ST_T_ARM = 1, ST_T_STATUS = 2, ST_T_DISARM = 3, ST_T_QUERY = 4,     /* as in pfdaq_supervisor.c */
    ST_T_HELLO = 16, ST_T_LISTEN = 17, ST_T_FEATURES = 18, ST_T_SET_LIMITS = 19, ST_T_RESET = 20,
    ST_T_TRIP_NOTICE = 21, ST_T_SPEAK = 22                              /* hart 4 to Linux, unsolicited */
};

enum st_state { ST_S_IDLE = 0, ST_S_ARMED = 1, ST_S_ALARM = 2, ST_S_SAFE = 3 };

enum st_reason {
    ST_R_DEADLINE = 1u, ST_R_SEQUENCE = 2u, ST_R_BAD_FRAMES = 4u, ST_R_DROPS = 8u,
    ST_R_PEAK = 16u, ST_R_MEAN = 32u
};

#define ST_NO_LIMIT     0xFFFFFFFFu

struct st_msg {
    uint32_t magic, type, burst, next_seq, errors, deadline_ms, max_lag;
    uint32_t x[9];
};

struct st_ack {
    uint32_t magic, type, state, status_rx, deadline_miss, seq_mismatch;
    uint32_t fabric_seq, fabric_frames, uptime_ms;
    uint32_t events, last_reason, trips;
    char text[16];
};

_Static_assert(sizeof(struct st_msg) == 64, "st_msg is 16 words");
_Static_assert(sizeof(struct st_ack) == 64, "st_ack is 16 words");

struct st_limits { uint32_t max_bad, max_drops, max_peak, max_mean; };

/* what the environment provides */
struct st_io {
    void (*print)(const char *fmt, ...);                        /* one console line, no newline in fmt */
    void (*abort_source)(void);                                 /* stop the data source (CTRL.ABORT) */
    void (*send)(uint32_t remote, const void *buf, uint32_t len);
    void (*delay_ms)(uint32_t ms);
};

struct st_ctx {
    struct st_io io;
    uint32_t state, deadline_ms, max_lag;
    struct st_limits lim;
    uint32_t status_rx, deadline_miss, seq_mismatch, events, trips, last_reason, windows;
    uint32_t last_remote;
    uint32_t hello_mask;
};

#define ST_FEATURE_PRINT_EVERY  10u     /* one console line per this many FEATURES messages */

static inline void st_init(struct st_ctx *c, const struct st_io *io)
{
    memset(c, 0, sizeof(*c));
    c->io = *io;
    c->lim.max_bad = c->lim.max_drops = c->lim.max_peak = c->lim.max_mean = ST_NO_LIMIT;
}

static inline const char *st_state_name(uint32_t s)
{
    static const char *const n[] = { "IDLE", "ARMED", "ALARM", "SAFE" };
    return s < 4 ? n[s] : "?";
}

static inline void st_reason_str(uint32_t r, char *out, unsigned n)
{
    static const struct { uint32_t bit; const char *name; } t[] = {
        { ST_R_DEADLINE, "DEADLINE" }, { ST_R_SEQUENCE, "SEQUENCE" }, { ST_R_BAD_FRAMES, "BAD_FRAMES" },
        { ST_R_DROPS, "DROPS" }, { ST_R_PEAK, "PEAK" }, { ST_R_MEAN, "MEAN" }
    };
    unsigned len = 0, i;
    out[0] = 0;
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        const char *s = t[i].name;
        if (!(r & t[i].bit))
            continue;
        if (len && len + 1 < n)
            out[len++] = '+';
        while (*s && len + 1 < n)
            out[len++] = *s++;
        out[len] = 0;
    }
    if (!len)
        strncpy(out, "NONE", n);
}

static inline void st_fill_ack(const struct st_ctx *c, uint32_t type, uint32_t fabric_seq, uint32_t fabric_frames,
                               uint32_t now_ms, const char *text, struct st_ack *a)
{
    memset(a, 0, sizeof(*a));
    a->magic = ST_ACK_MAGIC;
    a->type = type;
    a->state = c->state;
    a->status_rx = c->status_rx;
    a->deadline_miss = c->deadline_miss;
    a->seq_mismatch = c->seq_mismatch;
    a->fabric_seq = fabric_seq;
    a->fabric_frames = fabric_frames;
    a->uptime_ms = now_ms;
    a->events = c->events;
    a->last_reason = c->last_reason;
    a->trips = c->trips;
    if (text)
        strncpy(a->text, text, sizeof(a->text));
}

/*
 * The trip: the one place where the system goes to SAFE. Stops the data source through the
 * fabric, records the reason, prints one line and tells Linux (unsolicited message to the
 * endpoint that talked to us last). A second trip while already SAFE does nothing.
 */
static inline void st_trip(struct st_ctx *c, uint32_t reasons, uint32_t fabric_seq,
                           uint32_t fabric_frames, uint32_t now_ms)
{
    char r[48];
    struct st_ack a;

    if (c->state == ST_S_SAFE)
        return;
    c->io.abort_source();
    c->state = ST_S_SAFE;
    c->trips++;
    c->last_reason = reasons;
    st_reason_str(reasons, r, sizeof(r));
    c->io.print("[station] TRIP #%u: %s at t=%u ms, source aborted, state SAFE",
                (unsigned)c->trips, r, (unsigned)now_ms);
    st_fill_ack(c, ST_T_TRIP_NOTICE, fabric_seq, fabric_frames, now_ms, "TRIP", &a);
    c->io.send(c->last_remote, &a, sizeof(a));
}

/* the receive call timed out: if armed, that is a missed deadline (ALARM, then TRIP) */
static inline void st_on_timeout(struct st_ctx *c, uint32_t fabric_seq, uint32_t fabric_frames, uint32_t now_ms)
{
    if (c->state != ST_S_ARMED)
        return;
    c->deadline_miss++;
    c->state = ST_S_ALARM;
    c->io.print("[station] ALARM: no message within %u ms (miss #%u, t=%u ms)",
                (unsigned)c->deadline_ms, (unsigned)c->deadline_miss, (unsigned)now_ms);
    st_trip(c, ST_R_DEADLINE, fabric_seq, fabric_frames, now_ms);
}

/* receive timeout for the supervisor loop: the deadline while armed, otherwise `block` */
static inline uint32_t st_timeout(const struct st_ctx *c, uint32_t block)
{
    return c->state == ST_S_ARMED ? c->deadline_ms : block;
}

static inline void st_features(struct st_ctx *c, const struct st_msg *m, uint32_t fseq, uint32_t fframes, uint32_t now)
{
    uint32_t why = 0;

    c->windows++;
    if (c->state != ST_S_ARMED)
        return;
    if (m->x[2] > c->lim.max_bad)   why |= ST_R_BAD_FRAMES;
    if (m->x[3] > c->lim.max_drops) why |= ST_R_DROPS;
    if (m->x[4] > c->lim.max_peak)  why |= ST_R_PEAK;
    if (m->x[5] > c->lim.max_mean)  why |= ST_R_MEAN;
    if (why || (c->windows % ST_FEATURE_PRINT_EVERY) == 0)
        c->io.print("[station] t=%u ms window %u: frames %u, bad %u, drops %u, peak %u, mean %u",
                    (unsigned)now, (unsigned)m->x[0], (unsigned)m->x[1], (unsigned)m->x[2], (unsigned)m->x[3],
                    (unsigned)m->x[4], (unsigned)m->x[5]);
    if (why)
        st_trip(c, why, fseq, fframes, now);
}

/* Handle one 64-byte message from Linux and answer it (and, for LISTEN, speak). */
static inline void st_handle(struct st_ctx *c, const struct st_msg *m, uint32_t remote,
                             uint32_t fabric_seq, uint32_t fabric_frames, uint32_t now_ms)
{
    struct st_ack a;
    const char *reply_text = NULL;
    uint32_t speak = 0;

    c->last_remote = remote;
    c->events++;

    switch (m->type) {
    case ST_T_ARM:
        c->deadline_ms = m->deadline_ms ? m->deadline_ms : 1000u;
        c->max_lag = m->max_lag;
        c->status_rx = c->deadline_miss = c->seq_mismatch = 0;
        c->state = ST_S_ARMED;
        c->io.print("[station] ARMED: deadline %u ms, max lag %u frames, fabric seq %u, t=%u ms",
                    (unsigned)c->deadline_ms, (unsigned)c->max_lag, (unsigned)fabric_seq, (unsigned)now_ms);
        break;

    case ST_T_STATUS:
        c->status_rx++;
        if (c->state == ST_S_SAFE)
            c->io.abort_source();       /* an ABORT written between two runs is cleared by the next START: repeat it */
        if (c->state == ST_S_ARMED && (uint32_t)(fabric_seq - m->next_seq) > c->max_lag) {
            c->seq_mismatch++;
            c->io.print("[station] sequence mismatch: linux %u, fabric %u (#%u)",
                        (unsigned)m->next_seq, (unsigned)fabric_seq, (unsigned)c->seq_mismatch);
            st_trip(c, ST_R_SEQUENCE, fabric_seq, fabric_frames, now_ms);
        }
        break;

    case ST_T_DISARM:
        c->io.print("[station] DISARM from %s: %u status, %u windows, %u deadline misses, %u seq mismatches, %u trips",
                    st_state_name(c->state), (unsigned)c->status_rx, (unsigned)c->windows,
                    (unsigned)c->deadline_miss, (unsigned)c->seq_mismatch, (unsigned)c->trips);
        c->state = ST_S_IDLE;
        break;

    case ST_T_FEATURES:
        st_features(c, m, fabric_seq, fabric_frames, now_ms);
        break;

    case ST_T_SET_LIMITS:
        c->lim.max_bad = m->x[0];
        c->lim.max_drops = m->x[1];
        c->lim.max_peak = m->x[2];
        c->lim.max_mean = m->x[3];
        c->io.print("[station] limits per window: bad <= %u, drops <= %u, peak <= %u, mean <= %u",
                    (unsigned)c->lim.max_bad, (unsigned)c->lim.max_drops,
                    (unsigned)c->lim.max_peak, (unsigned)c->lim.max_mean);
        break;

    case ST_T_RESET:
        if (c->state == ST_S_SAFE) {
            char r[48];
            st_reason_str(c->last_reason, r, sizeof(r));
            c->state = ST_S_ARMED;
            c->io.print("[station] RESET from operator: trip #%u (%s) cleared, ARMED again, t=%u ms",
                        (unsigned)c->trips, r, (unsigned)now_ms);
        } else {
            c->io.print("[station] RESET while %s: nothing to clear", st_state_name(c->state));
        }
        break;

    case ST_T_HELLO: {
        char text[17];
        memcpy(text, &m->x[0], 16);
        text[16] = 0;
        if (m->next_seq < 3u)
            c->hello_mask |= 1u << m->next_seq;
        c->io.print("[roll-call] hart 4 (FreeRTOS) got HELLO #%u from hart %u (Linux cpu %u): %s",
                    (unsigned)m->burst, (unsigned)m->errors, (unsigned)m->next_seq, text);
        if (c->hello_mask == 7u)
            c->io.print("[roll-call] heard all three Linux harts (1, 2, 3)");
        reply_text = "hart 4 FreeRTOS";
        break;
    }

    case ST_T_LISTEN:
        reply_text = "hart 4 FreeRTOS";
        speak = (c->hello_mask == 7u);
        if (!speak)
            c->io.print("[roll-call] LISTEN with mask %u of 7: nothing to say yet", (unsigned)c->hello_mask);
        break;

    default:    /* QUERY and anything else: answer only */
        break;
    }

    st_fill_ack(c, m->type, fabric_seq, fabric_frames, now_ms, reply_text, &a);
    if (m->type == ST_T_HELLO || m->type == ST_T_LISTEN)
        a.status_rx = (c->hello_mask & 1u) + ((c->hello_mask >> 1) & 1u) + ((c->hello_mask >> 2) & 1u);
    c->io.send(remote, &a, sizeof(a));

    if (speak) {
        c->io.delay_ms(500);
        st_fill_ack(c, ST_T_SPEAK, fabric_seq, fabric_frames, now_ms + 500u, "core 4 speaks", &a);
        c->io.print("[roll-call] core 4 speaks: sending one message of its own to Linux");
        c->io.send(remote, &a, sizeof(a));
        c->hello_mask = 0;
    }
}

#endif /* PFDAQ_STATION_H */
