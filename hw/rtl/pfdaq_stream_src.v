// pfdaq_stream_src - instrumented AXI4-Stream frame source with APB control.
//
// Project RTL (not vendor code). Port list follows the vendor AXI4_STREAM_DATA_GENERATOR
// of the Discovery Kit reference design v2026.04 so the vendor SmartDesign integration
// script could be reused (pfdaq_core wraps it together with the latency unit).
//
// The stream is 64 bits wide to match the DMA controller (AXI_DMA_DWIDTH=64): two 32-bit
// words per beat, even word in TDATA[31:0]. The CoreAXI4DMAController user guide requires
// all TKEEP/TSTRB bits set on every beat except the last, so an odd-length packet ends with
// a 4-byte beat (TKEEP=TSTRB=0x0F). In memory the frame is a plain little-endian word array.
// Within a packet TVALID stays high on every cycle (two words are generated per clock):
// on the board, CoreAXI4DMAController 2.2.107 corrupted and then hung on packets longer
// than about 360 bytes when the source offered a beat only every other clock (v0.2.1).
//
// Frame layout (32-bit words, sent LSB word first):
//   word 0      : MAGIC 0xA55A0000 | payload_words[15:0]
//   word 1      : sequence number (+1 per frame; also +1 per dropped frame in real-time mode)
//   word 2      : timestamp, ACLK cycle counter: when word 0 was offered (burst mode) or when
//                 the frame became due (real-time mode)
//   word 3..N+2 : payload, by MODE:
//                   0 COUNTER  word i = {seq[15:0], i[15:0]}
//                   1 PRBS     xorshift32 sequence seeded per frame with seq ^ 0x9E3779B9
//                              (seed 1 if that is 0); word i = state after i+1 steps
//                   2 FIR      8-tap FIR over a triangle wave, continuous across the frames
//                              of one run (see pfdaq_fir_src)
//   word N+3    : CRC-32 (IEEE 802.3, reflected, init/xorout 0xFFFFFFFF) over words 0..N+2,
//                 bytes taken little-endian (matches zlib.crc32 of the raw buffer). Computed
//                 over the intended payload, so an injected payload fault is detectable.
//
// Run modes:
//   burst      (MODE.REALTIME=0) START sends FRAME_COUNT frames back to back. TLAST on the last
//              frame only, or on every frame with CTRL.TLAST_PER_FRAME (then GAP_CYCLES idle
//              cycles between packets). The source waits on backpressure; nothing is lost.
//   real-time  (MODE.REALTIME=1) START opens FRAME_COUNT frame slots, one every RATE cycles,
//              like an ADC that cannot be paused. A slot becomes a frame (its own packet, TLAST
//              on its CRC word) only if the source is idle and holds a credit; otherwise the
//              frame is dropped: DROPS +1 and the sequence number still advances, so the
//              consumer sees the gap. Each sent frame uses one credit and the next of
//              TDEST_COUNT stream destinations (TDEST 0,1,..), so software can keep up to four
//              DMA stream descriptors armed and return credits as it re-arms them.
//
// Abort (CTRL.ABORT) always leaves the DMA with a completed transfer:
//   - real-time and TLAST_PER_FRAME modes: every frame is its own packet, so the frame in
//     progress is finished and the run stops.
//   - burst mode (one packet for the whole run): the frame in progress is finished, then the
//     rest of the run's FRAME_COUNT x (PAYLOAD_WORDS+4) words is sent as zero words, with TLAST
//     on the last one. Found on the board with CoreAXI4DMAController 2.2.107: an early TLAST does
//     not complete a stream transfer, only the descriptor's full byte count does, so a burst cut
//     short leaves the DMA waiting. FRAMES_SENT tells software how many frames are real.
// CTRL.FORCE_ABORT stops at once and may leave the DMA waiting (recovery needs a reset).
//
// APB register map (PCLK domain), offsets in bytes:
//   0x00 ID           RO  0x50464451 ("PFDQ")
//   0x04 VERSION      RO  0x00050000 (parameter)
//   0x08 CTRL         WO  bit0 START, bit1 SNAPSHOT, bit4 ABORT, bit5 FORCE_ABORT (pulses)
//                     RW  bit8 TLAST_PER_FRAME (burst mode)
//   0x0C PAYLOAD_WORDS RW payload words per frame, 1..65535
//   0x10 FRAME_COUNT  RW  frames (burst) or frame slots (real-time) per START
//   0x14 GAP_CYCLES   RW  idle ACLK cycles between packets (burst mode, TLAST_PER_FRAME)
//   0x18 FAULT        RW  bit0 corrupt CRC, bit1 skip one sequence number, bit2 flip payload bit
//                         [31:16] index of the sent frame (within the run) the fault applies to
//   0x1C STATUS       RO  bit0 busy, bit1 done (cleared by START), bit2 last run ended by an
//                         abort, bit3 FIR data underrun seen (should never be set)
//   0x20 FRAMES_SENT  RO  snapshot: frames sent since reset
//   0x24 WORDS_SENT   RO  snapshot: 32-bit words sent since reset
//   0x28 STALL_CYCLES RO  snapshot: cycles held off by backpressure since reset
//   0x2C TIMESTAMP    RO  snapshot: ACLK cycle counter
//   0x30 NEXT_SEQ     RO  snapshot: sequence number the next frame will carry
//   0x34 MODE         RW  [1:0] payload 0 COUNTER, 1 PRBS, 2 FIR; bit4 REALTIME;
//                         [9:8] TDEST_COUNT-1 (real-time mode)
//   0x38 RATE         RW  real-time frame interval, ACLK cycles (minimum 2)
//   0x3C CREDIT       W   [15:0] credits to add; bit31 set: replace instead of add
//                     R   snapshot: credits held
//   0x40 DROPS        RO  snapshot: real-time frames dropped since reset
//   0x44 FIR_STEP     RW  [15:0] triangle phase increment per sample
//   0x48..0x64 COEF0..COEF7 RW [15:0] signed FIR coefficients, tap 0 = newest sample
// Snapshot registers are captured in ACLK on SNAPSHOT and when a run ends, then copied into
// PCLK registers once a synchronised toggle says they are stable, so the APB read path
// contains PCLK flops only.
//
// Clock domains: APB registers in PCLK, stream logic in ACLK. Configuration is quasi-static
// (must not be written while busy) and is sampled in ACLK on the synchronised START. Pulses
// cross as toggles through 2-FF synchronisers; the CREDIT value is held stable in PCLK while
// its toggle crosses. The intended false paths are listed in pfdaq_cdc.sdc.

`timescale 1ns / 1ps

module pfdaq_stream_src #(
    parameter [31:0] VERSION_VALUE = 32'h00050000
) (
    input             ACLK,
    input             RSTN,
    input             TREADY,
    output     [1:0]  TDEST,
    output     [7:0]  TID,
    output     [7:0]  TKEEP,
    output            TLAST,
    output            TVALID,
    output     [7:0]  TSTRB,
    output     [63:0] TDATA,
    input             PCLK,
    input             PRESETN,
    input             PENABLE,
    input             PSEL,
    input      [31:0] PADDR,
    input             PWRITE,
    input      [31:0] PWDATA,
    output     [31:0] PRDATA,
    output            PREADY,
    output            PSLVERR
);

    localparam [31:0] ID_VALUE = 32'h50464451;

    // ------------------------------------------------------------------
    // APB side (PCLK)
    // ------------------------------------------------------------------
    reg         tlast_per_frame;
    reg  [15:0] payload_words;
    reg  [31:0] frame_count;
    reg  [31:0] gap_cycles;
    reg  [31:0] fault;
    reg  [1:0]  mode_payload;
    reg         mode_rt;
    reg  [1:0]  mode_tdn;
    reg  [31:0] rate;
    reg  [15:0] fir_step;
    reg  [127:0] coef_p;            // COEF0 in [15:0]
    reg  [16:0] credit_val_p;       // {replace, amount}
    reg         start_tgl_p, snap_tgl_p, abort_tgl_p, fabort_tgl_p, credit_tgl_p;
    reg         done_p;

    wire        apb_write = PSEL & PENABLE & PWRITE;
    wire [7:0]  addr      = {PADDR[7:2], 2'b00};

    // ACLK -> PCLK
    reg         busy_a;
    reg  [1:0]  busy_sync_p;
    reg  [31:0] snap_frames, snap_words, snap_stall, snap_time, snap_seq, snap_drops;
    reg  [15:0] snap_credits;
    reg  [1:0]  snap_flags;         // {underrun, aborted}
    reg         snap_tgl_a;
    reg         snap_is_done;
    reg  [2:0]  snapv_sync_p;
    wire        snapv_pulse_p = snapv_sync_p[2] ^ snapv_sync_p[1];
    reg  [31:0] snap_frames_p, snap_words_p, snap_stall_p, snap_time_p, snap_seq_p, snap_drops_p;
    reg  [15:0] snap_credits_p;
    reg  [1:0]  snap_flags_p;

    always @(posedge PCLK or negedge PRESETN) begin
        if (!PRESETN) begin
            tlast_per_frame <= 1'b0;
            payload_words   <= 16'd16;
            frame_count     <= 32'd1;
            gap_cycles      <= 32'd0;
            fault           <= 32'd0;
            mode_payload    <= 2'd0;
            mode_rt         <= 1'b0;
            mode_tdn        <= 2'd0;
            rate            <= 32'd1000;
            fir_step        <= 16'd256;
            coef_p          <= {16'd0, 16'd0, 16'd0, 16'd0, 16'd0, 16'd0, 16'd0, 16'd1};
            credit_val_p    <= 17'd0;
            start_tgl_p     <= 1'b0;
            snap_tgl_p      <= 1'b0;
            abort_tgl_p     <= 1'b0;
            fabort_tgl_p    <= 1'b0;
            credit_tgl_p    <= 1'b0;
            done_p          <= 1'b0;
            busy_sync_p     <= 2'b00;
            snapv_sync_p    <= 3'b000;
            snap_frames_p   <= 32'd0;
            snap_words_p    <= 32'd0;
            snap_stall_p    <= 32'd0;
            snap_time_p     <= 32'd0;
            snap_seq_p      <= 32'd0;
            snap_drops_p    <= 32'd0;
            snap_credits_p  <= 16'd0;
            snap_flags_p    <= 2'b00;
        end else begin
            snapv_sync_p <= {snapv_sync_p[1:0], snap_tgl_a};
            if (snapv_pulse_p) begin
                // source registers have been stable for at least two PCLK cycles here
                snap_frames_p  <= snap_frames;
                snap_words_p   <= snap_words;
                snap_stall_p   <= snap_stall;
                snap_time_p    <= snap_time;
                snap_seq_p     <= snap_seq;
                snap_drops_p   <= snap_drops;
                snap_credits_p <= snap_credits;
                snap_flags_p   <= snap_flags;
                // end-of-run snapshots carry the done flag over the same crossing
                if (snap_is_done)
                    done_p <= 1'b1;
            end
            busy_sync_p <= {busy_sync_p[0], busy_a};

            if (apb_write) begin
                case (addr)
                    8'h08: begin
                        tlast_per_frame <= PWDATA[8];
                        if (PWDATA[0]) begin
                            start_tgl_p <= ~start_tgl_p;
                            done_p      <= 1'b0;
                        end
                        if (PWDATA[1]) snap_tgl_p   <= ~snap_tgl_p;
                        if (PWDATA[4]) abort_tgl_p  <= ~abort_tgl_p;
                        if (PWDATA[5]) fabort_tgl_p <= ~fabort_tgl_p;
                    end
                    8'h0C: payload_words <= (PWDATA[15:0] == 16'd0) ? 16'd1 : PWDATA[15:0];
                    8'h10: frame_count   <= (PWDATA == 32'd0) ? 32'd1 : PWDATA;
                    8'h14: gap_cycles    <= PWDATA;
                    8'h18: fault         <= PWDATA;
                    8'h34: begin
                        mode_payload <= PWDATA[1:0];
                        mode_rt      <= PWDATA[4];
                        mode_tdn     <= PWDATA[9:8];
                    end
                    8'h38: rate          <= (PWDATA < 32'd2) ? 32'd2 : PWDATA;
                    8'h3C: begin
                        credit_val_p <= {PWDATA[31], PWDATA[15:0]};
                        credit_tgl_p <= ~credit_tgl_p;
                    end
                    8'h44: fir_step      <= PWDATA[15:0];
                    8'h48: coef_p[15:0]    <= PWDATA[15:0];
                    8'h4C: coef_p[31:16]   <= PWDATA[15:0];
                    8'h50: coef_p[47:32]   <= PWDATA[15:0];
                    8'h54: coef_p[63:48]   <= PWDATA[15:0];
                    8'h58: coef_p[79:64]   <= PWDATA[15:0];
                    8'h5C: coef_p[95:80]   <= PWDATA[15:0];
                    8'h60: coef_p[111:96]  <= PWDATA[15:0];
                    8'h64: coef_p[127:112] <= PWDATA[15:0];
                    default: ;
                endcase
            end
        end
    end

    reg [31:0] prdata_mux;
    always @(*) begin
        case (addr)
            8'h00:   prdata_mux = ID_VALUE;
            8'h04:   prdata_mux = VERSION_VALUE;
            8'h08:   prdata_mux = {23'd0, tlast_per_frame, 8'd0};
            8'h0C:   prdata_mux = {16'd0, payload_words};
            8'h10:   prdata_mux = frame_count;
            8'h14:   prdata_mux = gap_cycles;
            8'h18:   prdata_mux = fault;
            8'h1C:   prdata_mux = {28'd0, snap_flags_p, done_p, busy_sync_p[1]};
            8'h20:   prdata_mux = snap_frames_p;
            8'h24:   prdata_mux = snap_words_p;
            8'h28:   prdata_mux = snap_stall_p;
            8'h2C:   prdata_mux = snap_time_p;
            8'h30:   prdata_mux = snap_seq_p;
            8'h34:   prdata_mux = {22'd0, mode_tdn, 3'd0, mode_rt, 2'd0, mode_payload};
            8'h38:   prdata_mux = rate;
            8'h3C:   prdata_mux = {16'd0, snap_credits_p};
            8'h40:   prdata_mux = snap_drops_p;
            8'h44:   prdata_mux = {16'd0, fir_step};
            8'h48:   prdata_mux = {16'd0, coef_p[15:0]};
            8'h4C:   prdata_mux = {16'd0, coef_p[31:16]};
            8'h50:   prdata_mux = {16'd0, coef_p[47:32]};
            8'h54:   prdata_mux = {16'd0, coef_p[63:48]};
            8'h58:   prdata_mux = {16'd0, coef_p[79:64]};
            8'h5C:   prdata_mux = {16'd0, coef_p[95:80]};
            8'h60:   prdata_mux = {16'd0, coef_p[111:96]};
            8'h64:   prdata_mux = {16'd0, coef_p[127:112]};
            default: prdata_mux = 32'd0;
        endcase
    end

    assign PRDATA  = prdata_mux;
    assign PREADY  = 1'b1;
    assign PSLVERR = 1'b0;

    // ------------------------------------------------------------------
    // Stream side (ACLK)
    // ------------------------------------------------------------------
    reg  [2:0]  start_sync_a, snap_sync_a, abort_sync_a, fabort_sync_a, credit_sync_a;
    wire        start_pulse_a  = start_sync_a[2]  ^ start_sync_a[1];
    wire        snap_pulse_a   = snap_sync_a[2]   ^ snap_sync_a[1];
    wire        abort_pulse_a  = abort_sync_a[2]  ^ abort_sync_a[1];
    wire        fabort_pulse_a = fabort_sync_a[2] ^ fabort_sync_a[1];
    wire        credit_pulse_a = credit_sync_a[2] ^ credit_sync_a[1];

    // configuration sampled at START
    reg         cfg_tlast_pf, cfg_rt;
    reg  [1:0]  cfg_mode, cfg_tdn;
    reg  [15:0] cfg_payload, cfg_fault_idx, cfg_step;
    reg  [31:0] cfg_frames, cfg_gap, cfg_rate;
    reg  [2:0]  cfg_fault_en;
    reg  [127:0] cfg_coef;

    localparam [2:0] R_IDLE = 3'd0, R_PREFILL = 3'd1, R_RUN = 3'd2, R_GAP = 3'd3, R_RTWAIT = 3'd4, R_FILL = 3'd5;
    reg  [2:0]  run;

    reg  [2:0]  ph;
    reg  [15:0] pay_idx;
    reg  [31:0] seq, frame_idx, crc, frame_ts, timebase, gap_cnt, prbs;
    reg  [31:0] frames_sent, words_sent, stall_cycles, drops;
    reg  [15:0] credits, seq_adv;
    reg  [1:0]  tdest_idx;
    reg  [31:0] rt_timer, rt_slots;
    reg  [31:0] run_words, cfg_total;   // words sent in this run / words the run must send in total
    reg         abort_pend, pkt_open, underrun, aborted;

    // FIR sample source (only used in MODE 2)
    wire [31:0] fir_head0, fir_head1;
    wire [4:0]  fir_count;
    wire [1:0]  fir_pop;
    pfdaq_fir_src u_fir (
        .clk(ACLK), .rstn(RSTN), .restart(start_pulse_a | fabort_pulse_a),
        .run((run != R_IDLE) && (cfg_mode == 2'd2)),
        .step(cfg_step), .coef(cfg_coef), .pop(fir_pop),
        .head0(fir_head0), .head1(fir_head1), .count(fir_count));

    // two chained word steps per clock: A produces the even word, B the odd word
    // burst mode with a single packet per run: an abort is completed with zero fill
    wire        fill_mode  = !cfg_rt && !cfg_tlast_pf;
    wire [31:0] ts_new = cfg_rt ? frame_ts : timebase;
    wire [31:0] a_word, b_word;
    wire        a_plast, b_plast, a_fdone, b_fdone, a_bdone, b_bdone, a_magic, b_magic, a_uses, b_uses;
    wire [2:0]  a_ph, b_ph;
    wire [15:0] a_pay, b_pay;
    wire [31:0] a_seq, b_seq, a_fidx, b_fidx, a_crc, b_crc, a_prbs, b_prbs;

    pfdaq_word_step step_a (
        .ph(ph), .pay_idx(pay_idx), .seq(seq), .frame_idx(frame_idx), .crc(crc), .prbs(prbs),
        .frame_ts(frame_ts), .mode(cfg_mode), .pay_in(fir_head0), .force_last(abort_pend && !fill_mode),
        .cfg_tlast_pf(cfg_tlast_pf), .cfg_payload(cfg_payload), .cfg_frames(cfg_frames),
        .cfg_fault_en(cfg_fault_en), .cfg_fault_idx(cfg_fault_idx),
        .word(a_word), .pkt_last(a_plast), .frame_done(a_fdone), .burst_done(a_bdone),
        .is_magic(a_magic), .uses_pay(a_uses),
        .n_ph(a_ph), .n_pay_idx(a_pay), .n_seq(a_seq), .n_frame_idx(a_fidx), .n_crc(a_crc), .n_prbs(a_prbs));

    pfdaq_word_step step_b (
        .ph(a_ph), .pay_idx(a_pay), .seq(a_seq), .frame_idx(a_fidx), .crc(a_crc), .prbs(a_prbs),
        .frame_ts(a_magic ? ts_new : frame_ts), .mode(cfg_mode),
        .pay_in(a_uses ? fir_head1 : fir_head0), .force_last(abort_pend && !fill_mode),
        .cfg_tlast_pf(cfg_tlast_pf), .cfg_payload(cfg_payload), .cfg_frames(cfg_frames),
        .cfg_fault_en(cfg_fault_en), .cfg_fault_idx(cfg_fault_idx),
        .word(b_word), .pkt_last(b_plast), .frame_done(b_fdone), .burst_done(b_bdone),
        .is_magic(b_magic), .uses_pay(b_uses),
        .n_ph(b_ph), .n_pay_idx(b_pay), .n_seq(b_seq), .n_frame_idx(b_fidx), .n_crc(b_crc), .n_prbs(b_prbs));

    // registered output beat
    reg         out_valid;
    reg  [63:0] out_data;
    reg  [7:0]  out_keep;
    reg         out_last;
    reg  [1:0]  out_tdest;
    wire        w_ready = ~out_valid | TREADY;

    // values after this clock's one or two words
    wire        two        = ~a_plast;
    wire        e_plast    = two ? b_plast : 1'b1;
    wire        e_bdone    = two ? b_bdone : a_bdone;
    wire [1:0]  e_frames   = {1'b0, a_fdone} + {1'b0, two & b_fdone};
    wire [31:0] e_seq      = two ? b_seq : a_seq;
    wire [1:0]  fir_pop_need = {1'b0, a_uses} + {1'b0, two & b_uses};
    wire        fir_ok     = (cfg_mode != 2'd2) || ({3'd0, fir_pop_need} <= fir_count);
    // abort between frames with no packet open: stop without sending anything more
    wire        abort_now  = abort_pend && !fill_mode && !pkt_open && (ph == 3'd0);
    // in fill mode an abort at a frame boundary goes straight to the zero fill
    wire        abort_fill = abort_pend && fill_mode && (ph == 3'd0);
    // the frame in progress ends with word A while an abort is pending: its pair word is fill
    wire        split_a    = abort_pend && fill_mode && a_fdone && !a_bdone;
    wire [31:0] remaining  = cfg_total - run_words;
    wire        emit       = (run == R_RUN) && w_ready && fir_ok && !abort_now && !abort_fill;
    assign      fir_pop    = emit ? (split_a ? {1'b0, a_uses} : fir_pop_need) : 2'd0;

    // real-time slots
    wire        rt_active  = cfg_rt && ((run == R_RTWAIT) || (run == R_RUN));
    wire        due        = rt_active && (rt_timer == 32'd0);
    wire        can_start  = (run == R_RTWAIT) && (credits != 16'd0) && !abort_pend;
    wire        drop_now   = due && !can_start;
    wire        slots_end  = (rt_slots + {31'd0, due}) >= cfg_frames;
    wire [15:0] credit_add = credit_pulse_a ? credit_val_p[15:0] : 16'd0;

    assign TVALID = out_valid;
    assign TDATA  = out_data;
    assign TLAST  = out_last;
    assign TKEEP  = out_keep;
    assign TSTRB  = out_keep;
    assign TDEST  = out_tdest;
    assign TID    = 8'h00;

    task end_run;
        input is_abort;
        begin
            run          <= R_IDLE;
            busy_a       <= 1'b0;
            aborted      <= is_abort;
            abort_pend   <= 1'b0;
            snap_is_done <= 1'b1;
            snap_tgl_a   <= ~snap_tgl_a;
        end
    endtask

    // one beat of zero fill (called with w_ready); ends the run on the last one
    task fill_beat;
        begin
            run        <= R_FILL;
            out_valid  <= 1'b1;
            out_data   <= 64'd0;
            out_keep   <= (remaining == 32'd1) ? 8'h0F : 8'hFF;
            out_last   <= (remaining <= 32'd2);
            words_sent <= words_sent + ((remaining == 32'd1) ? 32'd1 : 32'd2);
            run_words  <= run_words + ((remaining == 32'd1) ? 32'd1 : 32'd2);
            pkt_open   <= (remaining > 32'd2);
            if (remaining <= 32'd2) begin
                end_run(1'b1);
                snap_frames  <= frames_sent;
                snap_words   <= words_sent + ((remaining == 32'd1) ? 32'd1 : 32'd2);
                snap_stall   <= stall_cycles;
                snap_time    <= timebase;
                snap_seq     <= seq;
                snap_drops   <= drops;
                snap_credits <= credits;
                snap_flags   <= {underrun, 1'b1};
            end
        end
    endtask

    always @(posedge ACLK or negedge RSTN) begin
        if (!RSTN) begin
            start_sync_a <= 3'b000; snap_sync_a <= 3'b000; abort_sync_a <= 3'b000;
            fabort_sync_a <= 3'b000; credit_sync_a <= 3'b000;
            cfg_tlast_pf <= 1'b0; cfg_rt <= 1'b0; cfg_mode <= 2'd0; cfg_tdn <= 2'd0;
            cfg_payload <= 16'd16; cfg_fault_idx <= 16'd0; cfg_step <= 16'd0;
            cfg_frames <= 32'd1; cfg_gap <= 32'd0; cfg_rate <= 32'd2; cfg_fault_en <= 3'b000;
            cfg_coef <= 128'd0;
            run <= R_IDLE; ph <= 3'd0; pay_idx <= 16'd0;
            seq <= 32'd0; frame_idx <= 32'd0; crc <= 32'hFFFFFFFF; frame_ts <= 32'd0;
            timebase <= 32'd0; gap_cnt <= 32'd0; prbs <= 32'd1;
            frames_sent <= 32'd0; words_sent <= 32'd0; stall_cycles <= 32'd0; drops <= 32'd0;
            credits <= 16'd0; seq_adv <= 16'd0; tdest_idx <= 2'd0;
            rt_timer <= 32'd0; rt_slots <= 32'd0; run_words <= 32'd0; cfg_total <= 32'd0;
            abort_pend <= 1'b0; pkt_open <= 1'b0; underrun <= 1'b0; aborted <= 1'b0;
            busy_a <= 1'b0;
            out_valid <= 1'b0; out_data <= 64'd0; out_keep <= 8'h00; out_last <= 1'b0; out_tdest <= 2'd0;
            snap_frames <= 32'd0; snap_words <= 32'd0; snap_stall <= 32'd0; snap_time <= 32'd0;
            snap_seq <= 32'd0; snap_drops <= 32'd0; snap_credits <= 16'd0; snap_flags <= 2'b00;
            snap_tgl_a <= 1'b0; snap_is_done <= 1'b0;
        end else begin
            start_sync_a  <= {start_sync_a[1:0],  start_tgl_p};
            snap_sync_a   <= {snap_sync_a[1:0],   snap_tgl_p};
            abort_sync_a  <= {abort_sync_a[1:0],  abort_tgl_p};
            fabort_sync_a <= {fabort_sync_a[1:0], fabort_tgl_p};
            credit_sync_a <= {credit_sync_a[1:0], credit_tgl_p};
            timebase      <= timebase + 32'd1;
            // total words of a burst run; computed every clock from the sampled configuration
            cfg_total     <= cfg_frames[22:0] * ({1'b0, cfg_payload} + 17'd4);

            if (out_valid & TREADY)
                out_valid <= 1'b0;
            if (((run == R_RUN) || (run == R_FILL)) & ~w_ready)
                stall_cycles <= stall_cycles + 32'd1;
            if ((run == R_RUN) && w_ready && !fir_ok)
                underrun <= 1'b1;

            // credits: added by software, one used per real-time frame
            if (credit_pulse_a && credit_val_p[16])
                credits <= credit_val_p[15:0] - {15'd0, due & can_start};
            else
                credits <= credits + credit_add - {15'd0, due & can_start};

            if (abort_pulse_a && (run != R_IDLE))
                abort_pend <= 1'b1;

            // snapshot (a run ending in the same clock overrides it below)
            if (snap_pulse_a) begin
                snap_frames  <= frames_sent;
                snap_words   <= words_sent;
                snap_stall   <= stall_cycles;
                snap_time    <= timebase;
                snap_seq     <= seq;
                snap_drops   <= drops;
                snap_credits <= credits;
                snap_flags   <= {underrun, aborted};
                snap_is_done <= 1'b0;
                snap_tgl_a   <= ~snap_tgl_a;
            end

            // real-time slot timer
            if (rt_active) begin
                rt_timer <= due ? cfg_rate - 32'd1 : rt_timer - 32'd1;
                if (due)
                    rt_slots <= rt_slots + 32'd1;
                if (drop_now)
                    drops <= drops + 32'd1;
            end

            case (run)
                R_IDLE: begin
                    if (start_pulse_a) begin
                        cfg_tlast_pf  <= tlast_per_frame | mode_rt;
                        cfg_rt        <= mode_rt;
                        cfg_mode      <= (mode_payload == 2'd3) ? 2'd0 : mode_payload;
                        cfg_tdn       <= mode_tdn;
                        cfg_payload   <= payload_words;
                        cfg_frames    <= frame_count;
                        cfg_gap       <= gap_cycles;
                        cfg_rate      <= rate;
                        cfg_fault_en  <= fault[2:0];
                        cfg_fault_idx <= fault[31:16];
                        cfg_step      <= fir_step;
                        cfg_coef      <= coef_p;
                        frame_idx     <= 32'd0;
                        ph            <= 3'd0;
                        pay_idx       <= 16'd0;
                        crc           <= 32'hFFFFFFFF;
                        busy_a        <= 1'b1;
                        aborted       <= 1'b0;
                        abort_pend    <= 1'b0;
                        pkt_open      <= 1'b0;
                        seq_adv       <= 16'd0;
                        tdest_idx     <= 2'd0;
                        out_tdest     <= 2'd0;
                        rt_slots      <= 32'd0;
                        rt_timer      <= rate - 32'd1;
                        run_words     <= 32'd0;
                        if (mode_payload == 2'd2)
                            run <= R_PREFILL;
                        else
                            run <= mode_rt ? R_RTWAIT : R_RUN;
                        // sequence-skip fault on frame 0 of the run
                        if (fault[1] && (fault[31:16] == 16'd0))
                            seq <= seq + 32'd1;
                    end
                end

                R_PREFILL: begin
                    // let the FIR pipeline fill so that a packet never waits for samples
                    if (abort_pend)
                        end_run(1'b1);
                    else if (fir_count >= 5'd12)
                        run <= cfg_rt ? R_RTWAIT : R_RUN;
                end

                R_RTWAIT: begin
                    if (drop_now)
                        seq <= seq + 32'd1;        // a dropped slot still uses a sequence number
                    if (abort_pend) begin
                        end_run(1'b1);
                    end else if (due && can_start) begin
                        run       <= R_RUN;
                        frame_ts  <= timebase;
                        out_tdest <= tdest_idx;
                        tdest_idx <= (tdest_idx == cfg_tdn) ? 2'd0 : tdest_idx + 2'd1;
                    end else if (slots_end && (due || rt_slots >= cfg_frames)) begin
                        end_run(1'b0);
                    end
                end

                R_RUN: begin
                    if (abort_now) begin
                        end_run(1'b1);
                    end else if (abort_fill) begin
                        // at a frame boundary: the first fill beat goes out in this same clock,
                        // so TVALID has no gap (the DMA needs a continuous packet)
                        if (remaining == 32'd0)
                            end_run(1'b1);
                        else if (w_ready)
                            fill_beat;
                        else
                            run <= R_FILL;
                    end else if (emit && split_a) begin
                        // last word of the aborted frame plus the first fill word
                        out_valid   <= 1'b1;
                        out_data    <= {32'd0, a_word};
                        out_keep    <= 8'hFF;
                        out_last    <= (remaining == 32'd2);
                        ph          <= a_ph;
                        pay_idx     <= a_pay;
                        frame_idx   <= a_fidx;
                        crc         <= a_crc;
                        prbs        <= a_prbs;
                        seq         <= a_seq;
                        words_sent  <= words_sent + 32'd2;
                        run_words   <= run_words + 32'd2;
                        frames_sent <= frames_sent + 32'd1;
                        pkt_open    <= (remaining != 32'd2);
                        if (remaining == 32'd2) begin
                            end_run(1'b1);
                            snap_frames  <= frames_sent + 32'd1;
                            snap_words   <= words_sent + 32'd2;
                            snap_stall   <= stall_cycles;
                            snap_time    <= timebase;
                            snap_seq     <= a_seq;
                            snap_drops   <= drops;
                            snap_credits <= credits;
                            snap_flags   <= {underrun, 1'b1};
                        end else begin
                            run <= R_FILL;
                        end
                    end else if (emit) begin
                        out_valid   <= 1'b1;
                        out_data    <= two ? {b_word, a_word} : {32'd0, a_word};
                        out_keep    <= two ? 8'hFF : 8'h0F;
                        out_last    <= e_plast;
                        ph          <= two ? b_ph   : a_ph;
                        pay_idx     <= two ? b_pay  : a_pay;
                        frame_idx   <= two ? b_fidx : a_fidx;
                        crc         <= two ? b_crc  : a_crc;
                        prbs        <= two ? b_prbs : a_prbs;
                        if (!cfg_rt && (a_magic | (two & b_magic)))
                            frame_ts <= timebase;
                        words_sent  <= words_sent + (two ? 32'd2 : 32'd1);
                        run_words   <= run_words + (two ? 32'd2 : 32'd1);
                        frames_sent <= frames_sent + {30'd0, e_frames};
                        pkt_open    <= ~e_plast;
                        gap_cnt     <= 32'd0;
                        // real-time drops during this frame are folded into the next sequence number
                        if (cfg_rt && e_plast) begin
                            seq     <= e_seq + {16'd0, seq_adv} + {31'd0, drop_now};
                            seq_adv <= 16'd0;
                        end else begin
                            seq     <= e_seq;
                            if (drop_now) seq_adv <= seq_adv + 16'd1;
                        end
                        if (cfg_rt) begin
                            if (e_plast) begin
                                if (abort_pend || slots_end)
                                    end_run(abort_pend);
                                else
                                    run <= R_RTWAIT;
                            end
                        end else if (e_bdone) begin
                            end_run(abort_pend);
                        end else if (e_plast && (cfg_gap != 32'd0)) begin
                            run <= R_GAP;
                        end else if (fill_mode && abort_pend && two && b_fdone) begin
                            run <= R_FILL;              // aborted frame ended with word B
                        end
                        if (!cfg_rt && e_bdone || cfg_rt && e_plast && (abort_pend || slots_end)) begin
                            // end-of-run snapshot with the values after this clock
                            snap_frames  <= frames_sent + {30'd0, e_frames};
                            snap_words   <= words_sent + (two ? 32'd2 : 32'd1);
                            snap_stall   <= stall_cycles;
                            snap_time    <= timebase;
                            snap_seq     <= (cfg_rt) ? e_seq + {16'd0, seq_adv} + {31'd0, drop_now} : e_seq;
                            snap_drops   <= drops + {31'd0, drop_now};
                            snap_credits <= credits;
                            snap_flags   <= {underrun, abort_pend};
                        end
                    end else if (drop_now) begin
                        seq_adv <= seq_adv + 16'd1;
                    end
                end

                R_FILL: begin
                    // zero words up to the run's full length, TLAST on the last one
                    if (remaining == 32'd0)
                        end_run(1'b1);
                    else if (w_ready)
                        fill_beat;
                end

                R_GAP: begin
                    if (abort_now)
                        end_run(1'b1);
                    else if (gap_cnt >= cfg_gap - 32'd1)
                        run <= R_RUN;
                    else
                        gap_cnt <= gap_cnt + 32'd1;
                end

                default: run <= R_IDLE;
            endcase

            // end-of-run snapshot for runs that end outside R_RUN
            if (((run == R_RTWAIT) && (abort_pend || (!(due && can_start) && slots_end && (due || rt_slots >= cfg_frames))))
                || ((run == R_PREFILL) && abort_pend) || ((run == R_GAP) && abort_now)) begin
                snap_frames  <= frames_sent;
                snap_words   <= words_sent;
                snap_stall   <= stall_cycles;
                snap_time    <= timebase;
                snap_seq     <= ((run == R_RTWAIT) && drop_now) ? seq + 32'd1 : seq;
                snap_drops   <= drops + {31'd0, (run == R_RTWAIT) && drop_now};
                snap_credits <= credits;
                snap_flags   <= {underrun, abort_pend};
            end
            if ((run == R_RUN) && abort_now) begin
                snap_frames  <= frames_sent;
                snap_words   <= words_sent;
                snap_stall   <= stall_cycles;
                snap_time    <= timebase;
                snap_seq     <= seq + {16'd0, seq_adv};
                snap_drops   <= drops + {31'd0, drop_now};
                snap_credits <= credits;
                snap_flags   <= {underrun, 1'b1};
                if (cfg_rt) seq <= seq + {16'd0, seq_adv} + {31'd0, drop_now};
            end

            // FORCE_ABORT: drop whatever is in flight and return to idle
            if (fabort_pulse_a) begin
                run          <= R_IDLE;
                busy_a       <= 1'b0;
                out_valid    <= 1'b0;
                aborted      <= 1'b1;
                abort_pend   <= 1'b0;
                pkt_open     <= 1'b0;
                ph           <= 3'd0;
            end
        end
    end

endmodule

// One word of the frame stream: purely combinational. Given the generator state it returns
// the word to send and the state after that word.
module pfdaq_word_step (
    input      [2:0]  ph,
    input      [15:0] pay_idx,
    input      [31:0] seq,
    input      [31:0] frame_idx,
    input      [31:0] crc,
    input      [31:0] prbs,
    input      [31:0] frame_ts,
    input      [1:0]  mode,
    input      [31:0] pay_in,
    input             force_last,
    input             cfg_tlast_pf,
    input      [15:0] cfg_payload,
    input      [31:0] cfg_frames,
    input      [2:0]  cfg_fault_en,
    input      [15:0] cfg_fault_idx,
    output reg [31:0] word,
    output            pkt_last,
    output            frame_done,
    output            burst_done,
    output            is_magic,
    output            uses_pay,
    output reg [2:0]  n_ph,
    output reg [15:0] n_pay_idx,
    output reg [31:0] n_seq,
    output reg [31:0] n_frame_idx,
    output reg [31:0] n_crc,
    output reg [31:0] n_prbs
);
    localparam [2:0] P_MAGIC = 3'd0, P_SEQ = 3'd1, P_TIME = 3'd2, P_PAY = 3'd3, P_CRC = 3'd4;
    localparam [15:0] MAGIC_HI = 16'hA55A;

    // CRC-32, reflected polynomial 0xEDB88320, 32 data bits per call, bit 0 first
    function [31:0] crc32_word;
        input [31:0] c;
        input [31:0] d;
        integer i;
        reg [31:0] r;
        begin
            r = c;
            for (i = 0; i < 32; i = i + 1) begin
                if (r[0] ^ d[i])
                    r = (r >> 1) ^ 32'hEDB88320;
                else
                    r = r >> 1;
            end
            crc32_word = r;
        end
    endfunction

    function [31:0] xorshift32;
        input [31:0] x;
        reg [31:0] t;
        begin
            t = x ^ (x << 13);
            t = t ^ (t >> 17);
            xorshift32 = t ^ (t << 5);
        end
    endfunction

    wire        fault_here   = (frame_idx[15:0] == cfg_fault_idx) && (frame_idx[31:16] == 16'd0);
    wire        last_frame   = (frame_idx == cfg_frames - 32'd1);
    wire        last_payload = (pay_idx == cfg_payload - 16'd1);
    wire [31:0] next_fidx    = frame_idx + 32'd1;
    wire        fault_next   = (next_fidx[15:0] == cfg_fault_idx) && (next_fidx[31:16] == 16'd0);
    wire [31:0] seed_x       = seq ^ 32'h9E3779B9;
    wire [31:0] prbs_next    = xorshift32(prbs);

    wire [31:0] pay_good  = (mode == 2'd1) ? prbs_next :
                            (mode == 2'd2) ? pay_in    : {seq[15:0], pay_idx};
    wire [31:0] pay_word  = (fault_here && cfg_fault_en[2] && (pay_idx == 16'd0)) ? (pay_good ^ 32'h00000100) : pay_good;
    wire [31:0] crc_final = ~crc;
    wire [31:0] crc_word  = (fault_here && cfg_fault_en[0]) ? (crc_final ^ 32'h00000001) : crc_final;

    reg  [31:0] crc_in;

    assign is_magic   = (ph == P_MAGIC);
    assign frame_done = (ph == P_CRC);
    assign pkt_last   = (ph == P_CRC) && (cfg_tlast_pf || last_frame || force_last);
    assign burst_done = (ph == P_CRC) && (last_frame || force_last);
    assign uses_pay   = (ph == P_PAY) && (mode == 2'd2);

    always @(*) begin
        word        = 32'd0;
        crc_in      = 32'd0;
        n_ph        = ph;
        n_pay_idx   = pay_idx;
        n_seq       = seq;
        n_frame_idx = frame_idx;
        n_prbs      = prbs;
        case (ph)
            P_MAGIC: begin word = {MAGIC_HI, cfg_payload}; crc_in = word; n_ph = P_SEQ; end
            P_SEQ:   begin word = seq;                     crc_in = word; n_ph = P_TIME; end
            P_TIME:  begin
                word = frame_ts; crc_in = word; n_ph = P_PAY; n_pay_idx = 16'd0;
                n_prbs = (seed_x == 32'd0) ? 32'd1 : seed_x;
            end
            P_PAY:   begin
                word = pay_word; crc_in = pay_good;
                n_pay_idx = pay_idx + 16'd1;
                n_prbs = prbs_next;
                if (last_payload) n_ph = P_CRC;
            end
            default: begin // P_CRC
                word        = crc_word;
                n_ph        = P_MAGIC;
                n_frame_idx = frame_idx + 32'd1;
                n_seq       = seq + 32'd1 + ((cfg_fault_en[1] && fault_next && !last_frame) ? 32'd1 : 32'd0);
            end
        endcase
        n_crc = (ph == P_CRC) ? 32'hFFFFFFFF : crc32_word(crc, crc_in);
    end
endmodule

// FIR sample source for MODE 2. Produces two filtered samples per clock into a 16-word FIFO
// that the frame generator pops from (0, 1 or 2 words per clock).
//   input  x[i]  = tri((i+1)*STEP mod 2^16), i = 0, 1, ... from START, where for a 16-bit phase p
//                  t = p[15] ? ~p[14:0] : p[14:0]  and  tri(p) = t - 16384  (signed 16 bit)
//   output y[n]  = sum_{k=0..7} COEF[k] * x[n-k], wrapped to 32 bits; x[n<0] = 0
// The phase starts at 0 and the history at 0 on every START; samples run on continuously
// across the frames of one run. Pipeline: window -> 16 products -> partial sums -> sums
// (4 stages). A credit rule keeps the FIFO from overflowing; after the generator's prefill it
// cannot run empty because the source produces as fast as the generator can consume.
module pfdaq_fir_src (
    input              clk,
    input              rstn,
    input              restart,
    input              run,
    input      [15:0]  step,
    input      [127:0] coef,
    input      [1:0]   pop,
    output reg [31:0]  head0,
    output reg [31:0]  head1,
    output reg [4:0]   count
);
    function signed [15:0] tri16;
        input [15:0] p;
        reg [14:0] t;
        begin
            t = p[15] ? ~p[14:0] : p[14:0];
            tri16 = $signed({1'b0, t}) - 16'sd16384;
        end
    endfunction

    reg  [15:0]        phase;
    reg  signed [15:0] h [0:6];             // h[0] = x[n-1] ... h[6] = x[n-7]
    reg  signed [15:0] w [0:8];             // stage 1 window: w[0]=x[n+1], w[1]=x[n], w[2..8]=x[n-1..n-7]
    reg  signed [31:0] p0 [0:7], p1 [0:7];  // stage 2 products
    reg  [31:0]        q0 [0:3], q1 [0:3];  // stage 3 partial sums
    reg  [31:0]        y0, y1;              // stage 4
    reg                v1, v2, v3, v4;
    reg  [31:0]        mem [0:15];
    reg  [3:0]         wp, rp;

    wire [2:0] inflight = {2'd0, v1} + {2'd0, v2} + {2'd0, v3} + {2'd0, v4};
    wire       issue    = run && ({1'b0, count} + {2'd0, inflight, 1'b0} + 6'd2 <= 6'd16);
    wire [15:0] ph_a    = phase + step;
    wire [15:0] ph_b    = phase + {step[14:0], 1'b0};

    wire signed [15:0] c [0:7];
    genvar g;
    generate
        for (g = 0; g < 8; g = g + 1) begin : coef_split
            assign c[g] = coef[16*g +: 16];
        end
    endgenerate

    // FIFO after this clock's push: used for the registered head words
    wire [3:0] rp_n  = rp + {2'd0, pop};
    wire [3:0] rp_n1 = rp_n + 4'd1;
    function [31:0] mem_after;
        input [3:0] a;
        begin
            if (v4 && (a == wp))               mem_after = y0;
            else if (v4 && (a == wp + 4'd1))   mem_after = y1;
            else                               mem_after = mem[a];
        end
    endfunction

    integer k;
    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            phase <= 16'd0; v1 <= 1'b0; v2 <= 1'b0; v3 <= 1'b0; v4 <= 1'b0;
            wp <= 4'd0; rp <= 4'd0; count <= 5'd0; head0 <= 32'd0; head1 <= 32'd0;
            y0 <= 32'd0; y1 <= 32'd0;
            for (k = 0; k < 7; k = k + 1) h[k] <= 16'sd0;
            for (k = 0; k < 9; k = k + 1) w[k] <= 16'sd0;
            for (k = 0; k < 8; k = k + 1) begin p0[k] <= 32'sd0; p1[k] <= 32'sd0; end
            for (k = 0; k < 4; k = k + 1) begin q0[k] <= 32'd0; q1[k] <= 32'd0; end
        end else if (restart) begin
            phase <= 16'd0; v1 <= 1'b0; v2 <= 1'b0; v3 <= 1'b0; v4 <= 1'b0;
            wp <= 4'd0; rp <= 4'd0; count <= 5'd0;
            for (k = 0; k < 7; k = k + 1) h[k] <= 16'sd0;
        end else begin
            // stage 1: two new samples and the window they need
            v1 <= issue;
            if (issue) begin
                w[0] <= tri16(ph_b);
                w[1] <= tri16(ph_a);
                for (k = 0; k < 7; k = k + 1) w[k + 2] <= h[k];
                h[0] <= tri16(ph_b);
                h[1] <= tri16(ph_a);
                for (k = 2; k < 7; k = k + 1) h[k] <= h[k - 2];
                phase <= ph_b;
            end
            // stage 2: products (y0 uses x[n]..x[n-7], y1 uses x[n+1]..x[n-6])
            v2 <= v1;
            for (k = 0; k < 8; k = k + 1) begin
                p0[k] <= c[k] * w[k + 1];
                p1[k] <= c[k] * w[k];
            end
            // stage 3 and 4: adder tree
            v3 <= v2;
            for (k = 0; k < 4; k = k + 1) begin
                q0[k] <= p0[2 * k] + p0[2 * k + 1];
                q1[k] <= p1[2 * k] + p1[2 * k + 1];
            end
            v4 <= v3;
            y0 <= (q0[0] + q0[1]) + (q0[2] + q0[3]);
            y1 <= (q1[0] + q1[1]) + (q1[2] + q1[3]);
            // FIFO
            if (v4) begin
                mem[wp]        <= y0;
                mem[wp + 4'd1] <= y1;
                wp             <= wp + 4'd2;
            end
            rp    <= rp_n;
            count <= count + (v4 ? 5'd2 : 5'd0) - {3'd0, pop};
            head0 <= mem_after(rp_n);
            head1 <= mem_after(rp_n1);
        end
    end
endmodule
