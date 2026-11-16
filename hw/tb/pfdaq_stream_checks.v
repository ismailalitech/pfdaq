// pfdaq_stream_checks - protocol and frame-structure checks for the stream interface of
// pfdaq_stream_src, written as monitors. Simulation only; nothing here is synthesised.
//
// The checker connects to PORTS only: the stream (TVALID, TREADY, TDATA, TKEEP, TSTRB, TLAST,
// TDEST, TID) and the APB bus. It never looks inside the design. The configuration of a run is
// taken from the register writes it sees on the APB bus (pfdaq_apb_snoop), exactly as software
// would have written them, so a bug in the design's own copy of the configuration cannot hide.
//
// Each rule has a number and a name. A violation prints one "CHECK FAIL" line (the first four of
// each rule) and is counted; pfdaq_stream_checks.report prints the totals and the verdict.
//
// Rules (18):
//   1 VALID_DROP    TVALID was high and TREADY low; the next clock TVALID is low again.
//   2 DATA_STABLE   ... TDATA changed.
//   3 SIDE_STABLE   ... TLAST, TKEEP, TSTRB, TDEST or TID changed.
//   4 KEEP          TKEEP equals TSTRB, and is 0xFF, or 0x0F on a TLAST beat.
//   5 VALID_GAP     TVALID low inside an open packet (after a beat without TLAST).
//   6 MAGIC         the first word of a frame is 0xA55A0000 | PAYLOAD_WORDS.
//   7 SEQ           the sequence number rises: by exactly 1 from frame to frame of a non-real-time run
//                   without the sequence-skip fault, by more than 0 otherwise (also across runs).
//   8 TIMESTAMP     the timestamp word rises from frame to frame.
//   9 TLAST_EARLY   TLAST before the last word of the packet.
//  10 TLAST_LATE    no TLAST on or after the last word of the packet.
//  11 TLAST_LONG    TLAST after the last word of the packet.
//  12 TDEST         burst: always 0; real-time: packet k of the run uses k mod TDEST_COUNT.
//  13 FILL_ZERO     after an abort of a burst, the rest of the packet is zero words.
//  14 RUN_PACKETS   packets in a run: burst 1 (0 or 1 if aborted); per-frame TLAST FRAME_COUNT
//                   (at most, if aborted); real-time at most FRAME_COUNT.
//  15 RUN_FRAMES    frames in a burst run: FRAME_COUNT (at most, if aborted).
//  16 PKT_OPEN     a run ends with its packet closed (unless FORCE_ABORT was used).
//  17 FORCE_STOP   after FORCE_ABORT TVALID is low within 8 clocks and stays low until START.
//  18 UNDERRUN     STATUS bit 3 (FIR data underrun) is never read as 1.
//
// A packet is: the whole burst; or one frame (per-frame TLAST, real-time). A frame is
// PAYLOAD_WORDS + 4 words: magic, sequence, timestamp, payload, CRC. The payload and CRC values
// are NOT checked here: that is the job of the independent Python model (check_trace.py).

`timescale 1ns / 1ps

// ---------------------------------------------------------------------------------------------
// Watches the APB bus (PCLK domain). Keeps its own copy of the registers the checks need, takes
// the run configuration at the START write, and turns CTRL pulses and a bad STATUS read into
// toggles for the ACLK domain.
// ---------------------------------------------------------------------------------------------
module pfdaq_apb_snoop (
    input             PCLK,
    input             PSEL,
    input             PENABLE,
    input             PWRITE,
    input      [31:0] PADDR,
    input      [31:0] PWDATA,
    input      [31:0] PRDATA,
    output reg [15:0] run_payload,
    output reg [31:0] run_frames,
    output reg [31:0] run_gap,
    output reg [31:0] run_rate,
    output reg        run_tlpf,       // one frame per packet: CTRL.TLAST_PER_FRAME or real-time
    output reg        run_rt,
    output reg [1:0]  run_mode,       // 0 counter, 1 PRBS, 2 FIR
    output reg [1:0]  run_tdn,        // TDEST_COUNT - 1
    output reg [2:0]  run_fault_en,
    output reg [15:0] run_fault_idx,
    output reg        start_tgl,
    output reg        abort_tgl,
    output reg        fabort_tgl,
    output reg        underrun_tgl
);
    reg [15:0] sh_payload = 16'd16;
    reg [31:0] sh_frames = 32'd1, sh_gap = 32'd0, sh_rate = 32'd1000, sh_fault = 32'd0;
    reg [1:0]  sh_mode = 2'd0, sh_tdn = 2'd0;
    reg        sh_rt = 1'b0;
    wire       wr = PSEL & PENABLE & PWRITE;
    wire       rd = PSEL & PENABLE & ~PWRITE;
    wire [7:0] a  = {PADDR[7:2], 2'b00};

    initial begin
        run_payload = 16'd16; run_frames = 32'd1; run_gap = 32'd0; run_rate = 32'd1000;
        run_tlpf = 1'b0; run_rt = 1'b0; run_mode = 2'd0; run_tdn = 2'd0;
        run_fault_en = 3'd0; run_fault_idx = 16'd0;
        start_tgl = 1'b0; abort_tgl = 1'b0; fabort_tgl = 1'b0; underrun_tgl = 1'b0;
    end

    always @(posedge PCLK) begin
        if (wr) begin
            case (a)
                8'h08: begin
                    if (PWDATA[0]) begin
                        run_payload   <= sh_payload;
                        run_frames    <= sh_frames;
                        run_gap       <= sh_gap;
                        run_rate      <= sh_rate;
                        run_tlpf      <= PWDATA[8] | sh_rt;
                        run_rt        <= sh_rt;
                        run_mode      <= (sh_mode == 2'd3) ? 2'd0 : sh_mode;
                        run_tdn       <= sh_tdn;
                        run_fault_en  <= sh_fault[2:0];
                        run_fault_idx <= sh_fault[31:16];
                        start_tgl     <= ~start_tgl;
                    end
                    if (PWDATA[4]) abort_tgl  <= ~abort_tgl;
                    if (PWDATA[5]) fabort_tgl <= ~fabort_tgl;
                end
                8'h0C: sh_payload <= (PWDATA[15:0] == 16'd0) ? 16'd1 : PWDATA[15:0];
                8'h10: sh_frames  <= (PWDATA == 32'd0) ? 32'd1 : PWDATA;
                8'h14: sh_gap     <= PWDATA;
                8'h18: sh_fault   <= PWDATA;
                8'h34: begin sh_mode <= PWDATA[1:0]; sh_rt <= PWDATA[4]; sh_tdn <= PWDATA[9:8]; end
                8'h38: sh_rate    <= (PWDATA < 32'd2) ? 32'd2 : PWDATA;
                default: ;
            endcase
        end
        if (rd && a == 8'h1C && PRDATA[3]) underrun_tgl <= ~underrun_tgl;
    end
endmodule

// ---------------------------------------------------------------------------------------------
// The stream checker (ACLK domain).
// ---------------------------------------------------------------------------------------------
module pfdaq_stream_checks (
    input             ACLK,
    input             RSTN,
    input             TVALID,
    input             TREADY,
    input      [63:0] TDATA,
    input      [7:0]  TKEEP,
    input      [7:0]  TSTRB,
    input             TLAST,
    input      [1:0]  TDEST,
    input      [7:0]  TID,
    input      [31:0] tag,            // scenario number, used only in messages
    // run configuration and events from pfdaq_apb_snoop
    input      [15:0] run_payload,
    input      [31:0] run_frames,
    input             run_tlpf,
    input             run_rt,
    input      [1:0]  run_tdn,
    input      [2:0]  run_fault_en,
    input             start_tgl,
    input             abort_tgl,
    input             fabort_tgl,
    input             underrun_tgl,
    // state and events for the coverage module (registered: valid one clock after the cause)
    output reg        pkt_open_o,
    output reg [31:0] beats_in_run_o,
    output reg        ev_beat,
    output reg [15:0] ev_wait,        // clocks the beat waited with TVALID high and TREADY low
    output reg        ev_first,       // first beat of a packet
    output reg        ev_last,        // TLAST beat
    output reg        ev_odd,         // TLAST beat with TKEEP 0x0F (a 4-byte last beat)
    output reg [31:0] ev_idle,        // clocks with TVALID low before this beat
    output reg        ev_pkt_gap_v,   // a packet start that is not the first of its run
    output reg        ev_frame,       // a frame header was seen
    output reg        ev_frame_inpkt, // ... and it did not start a packet
    output reg        ev_frame_hi,    // ... and it was in the high word of a beat
    output reg [1:0]  ev_tdest,       // TDEST of a packet start
    output reg        ev_seqgap_v,    // a real-time frame whose predecessor is in the same run
    output reg [31:0] ev_seqgap,      // sequence numbers skipped before it (dropped slots)
    output reg        ev_forced_stop
);
    // ---- rule table ----
    localparam NR = 19;
    integer cnt [0:NR-1];
    integer shown [0:NR-1];
    integer total_viol;
    reg [4:0] v_now = 0;           // lowest rule number reported at the latest clock edge, 0 if none (for waveforms)
    integer ri;
    reg [8*14-1:0] rname [0:NR-1];
    initial begin
        for (ri = 0; ri < NR; ri = ri + 1) begin cnt[ri] = 0; shown[ri] = 0; end
        total_viol = 0;
        rname[1]  = "VALID_DROP";   rname[2]  = "DATA_STABLE";  rname[3]  = "SIDE_STABLE";
        rname[4]  = "KEEP";         rname[5]  = "VALID_GAP";    rname[6]  = "MAGIC";
        rname[7]  = "SEQ";          rname[8]  = "TIMESTAMP";    rname[9]  = "TLAST_EARLY";
        rname[10] = "TLAST_LATE";   rname[11] = "TLAST_LONG";   rname[12] = "TDEST";
        rname[13] = "FILL_ZERO";    rname[14] = "RUN_PACKETS";  rname[15] = "RUN_FRAMES";
        rname[16] = "PKT_OPEN";     rname[17] = "FORCE_STOP";   rname[18] = "UNDERRUN";
    end

    function void viol(input integer r, input [31:0] info);
        begin
            cnt[r] = cnt[r] + 1;
            total_viol = total_viol + 1;
            if (v_now == 0 || r < v_now) v_now = r;
            if (shown[r] < 4) begin
                shown[r] = shown[r] + 1;
                $display("CHECK FAIL rule %0d %0s at %0d ns, scenario %0d, info 0x%08x",
                         r, rname[r], $time, tag, info);
            end
        end
    endfunction

    // ---- state ----
    reg        armed = 0;
    reg        start_q, abort_q, fabort_q, under_q;
    // stall tracking
    reg        p_stall = 0;
    reg [63:0] p_data;
    reg        p_last;
    reg [7:0]  p_keep, p_strb, p_id;
    reg [1:0]  p_dest;
    reg [15:0] waitc = 0;
    reg [31:0] idle_clks = 0;
    // force-abort window
    reg [3:0]  fwin = 0;           // counts down from 8 after FORCE_ABORT
    reg        forced = 0;         // FORCE_ABORT seen since the last START
    // run state
    reg        run_active = 0;
    reg        sv_tlpf = 0, sv_rt = 0;   // configuration of the run being finished
    reg [31:0] sv_frames = 0;
    reg        abort_req = 0;
    reg        pkt_open = 0;
    reg [31:0] wip = 0;            // words so far in the open packet
    reg [31:0] fpos = 0;           // word index inside the current frame
    reg        fill = 0;
    reg [31:0] pkts = 0, frames_seen = 0, beats = 0;
    reg        have_seq = 0;       // a frame of this run has been seen
    reg [31:0] prev_seq = 0, prev_ts = 0;
    reg        glob_seq_v = 0, glob_ts_v = 0;
    reg [31:0] glob_seq = 0, glob_ts = 0;
    // working variables
    reg [31:0] nw, wip_after, pk_dest_exp;
    reg        beat_first;

    // frame length in words, packet length in words (from the configuration seen on APB)
    wire [31:0] L    = {16'd0, run_payload} + 32'd4;
    wire [31:0] plen = run_tlpf ? L : run_frames * L;

    function void finish_run;
        reg exact;
        begin
            if (run_active) begin
                if (pkt_open && !forced) viol(16, wip);
                exact = !abort_req && !forced;
                if (!sv_tlpf) begin
                    if (pkts > 1 || (exact && pkts != 1)) viol(14, pkts);
                    if (frames_seen > sv_frames || (exact && frames_seen != sv_frames)) viol(15, frames_seen);
                end else if (sv_rt) begin
                    if (pkts > sv_frames) viol(14, pkts);
                end else begin
                    if (pkts > sv_frames || (exact && pkts != sv_frames)) viol(14, pkts);
                end
            end
        end
    endfunction

    // one 32-bit word of the stream, in order; fpos is the index inside the frame
    task word(input [31:0] x, input first_in_pkt, input hi);
        begin
            if (fill) begin
                if (x !== 32'd0) viol(13, x);
            end else begin
                if (fpos == 0) begin
                    if (abort_req && !run_tlpf && x === 32'd0) begin
                        fill = 1'b1;        // the zero fill that completes an aborted burst
                    end else begin
                        if (x !== {16'hA55A, run_payload}) viol(6, x);
                        frames_seen = frames_seen + 1;
                        ev_frame <= 1'b1;
                        if (!first_in_pkt) ev_frame_inpkt <= 1'b1;
                        if (hi) ev_frame_hi <= 1'b1;
                    end
                end else if (fpos == 1) begin
                    if (glob_seq_v && x <= glob_seq) viol(7, x);
                    else if (have_seq && !run_rt && !run_fault_en[1] && x !== prev_seq + 32'd1) viol(7, x);
                    if (have_seq && run_rt && x > prev_seq) begin
                        ev_seqgap_v <= 1'b1; ev_seqgap <= x - prev_seq - 32'd1;
                    end
                    prev_seq = x; glob_seq = x; glob_seq_v = 1'b1; have_seq = 1'b1;
                end else if (fpos == 2) begin
                    if (glob_ts_v && x <= glob_ts) viol(8, x);
                    glob_ts = x; glob_ts_v = 1'b1;
                end
            end
            fpos = fpos + 1;
            if (fpos == L) fpos = 0;
        end
    endtask

    always @(posedge ACLK) begin
        ev_beat <= 1'b0; ev_first <= 1'b0; ev_last <= 1'b0; ev_odd <= 1'b0; ev_pkt_gap_v <= 1'b0;
        ev_frame <= 1'b0; ev_frame_inpkt <= 1'b0; ev_frame_hi <= 1'b0; ev_seqgap_v <= 1'b0;
        ev_forced_stop <= 1'b0;
        v_now = 0;
        if (RSTN !== 1'b1) begin
            armed = 0; p_stall = 0; pkt_open = 0; run_active = 0; waitc = 0; idle_clks = 0;
        end else if (!armed) begin
            armed = 1; start_q = start_tgl; abort_q = abort_tgl; fabort_q = fabort_tgl; under_q = underrun_tgl;
        end else begin
            // ---- events from the APB side ----
            if (underrun_tgl !== under_q) begin under_q = underrun_tgl; viol(18, 32'd0); end
            if (fabort_tgl !== fabort_q) begin
                fabort_q = fabort_tgl; fwin = 4'd8; forced = 1'b1; ev_forced_stop <= 1'b1;
                pkt_open = 0; wip = 0; fpos = 0; p_stall = 0;
            end
            if (abort_tgl !== abort_q) begin abort_q = abort_tgl; if (run_active) abort_req = 1'b1; end
            if (start_tgl !== start_q) begin
                start_q = start_tgl;
                finish_run();
                sv_tlpf = run_tlpf; sv_rt = run_rt; sv_frames = run_frames;
                run_active = 1; abort_req = 0; forced = 0; fwin = 0; fill = 0;
                pkt_open = 0; wip = 0; fpos = 0; pkts = 0; frames_seen = 0; beats = 0; have_seq = 0;
                p_stall = 0; idle_clks = 0;
            end

            // ---- stall rules: what was true last clock decides what must hold now ----
            if (p_stall && fwin == 0) begin
                if (TVALID !== 1'b1) viol(1, 32'd0);
                else begin
                    if (TDATA !== p_data) viol(2, TDATA[31:0]);
                    if (TLAST !== p_last || TKEEP !== p_keep || TSTRB !== p_strb || TDEST !== p_dest || TID !== p_id)
                        viol(3, {16'd0, TKEEP, TSTRB});
                end
            end
            if (TVALID === 1'b1 && (TKEEP !== TSTRB || !(TKEEP === 8'hFF || (TLAST === 1'b1 && TKEEP === 8'h0F))))
                viol(4, {16'd0, TKEEP, TSTRB});
            if (pkt_open && TVALID !== 1'b1 && fwin == 0) viol(5, wip);
            if (forced && fwin == 0 && TVALID === 1'b1) viol(17, 32'd0);
            if (fwin != 0) fwin = fwin - 1;

            // ---- stall bookkeeping ----
            p_stall = (TVALID === 1'b1 && TREADY !== 1'b1);
            p_data = TDATA; p_last = TLAST; p_keep = TKEEP; p_strb = TSTRB; p_dest = TDEST; p_id = TID;

            // ---- an accepted beat ----
            if (TVALID === 1'b1 && TREADY === 1'b1) begin
                nw = (TKEEP === 8'hFF) ? 32'd2 : 32'd1;
                beat_first = !pkt_open;
                ev_beat <= 1'b1; ev_wait <= waitc; ev_idle <= idle_clks;
                ev_first <= beat_first; ev_last <= (TLAST === 1'b1);
                ev_odd <= (TLAST === 1'b1 && TKEEP === 8'h0F);
                beats = beats + 1;
                if (beat_first) begin
                    fpos = 0; wip = 0;
                    ev_pkt_gap_v <= (pkts != 0); ev_tdest <= TDEST;
                    pk_dest_exp = run_rt ? (pkts % ({30'd0, run_tdn} + 32'd1)) : 32'd0;
                end
                if (TDEST !== pk_dest_exp[1:0]) viol(12, {30'd0, TDEST});
                word(TDATA[31:0], beat_first, 1'b0);
                if (nw == 2) word(TDATA[63:32], 1'b0, 1'b1);
                wip_after = wip + nw;
                if (TLAST === 1'b1) begin
                    if (wip_after < plen) viol(9, wip_after);
                    else if (wip_after > plen) viol(11, wip_after);
                    pkt_open = 0; wip = 0; pkts = pkts + 1;
                    fpos = 0; fill = 0;
                end else begin
                    if (wip_after >= plen) viol(10, wip_after);
                    pkt_open = 1; wip = wip_after;
                end
                waitc = 0; idle_clks = 0;
            end else begin
                if (TVALID === 1'b1) begin if (waitc != 16'hFFFF) waitc = waitc + 1; end
                else begin idle_clks = idle_clks + 1; waitc = 0; end
            end
        end
        pkt_open_o <= pkt_open; beats_in_run_o <= beats;
    end

    function void report;
        integer r;
        begin
            finish_run();
            $display("CHECK rule summary: violations per rule over the whole run");
            for (r = 1; r < NR; r = r + 1)
                $display("CHECK rule %2d %-12s %0d", r, rname[r], cnt[r]);
            $display("CHECK total violations %0d", total_viol);
        end
    endfunction
endmodule
