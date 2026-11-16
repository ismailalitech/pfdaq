// pfdaq_stream_cover - functional coverage of the stream source, as plain counters.
// Simulation only. Icarus Verilog has no covergroups, so each "bin" is a name, a rule for when it
// counts, and a counter. The bins were written down before the first run; none was added or
// changed to make the table look better. A bin with count 0 is a behaviour the scenarios never
// reach, and the table says so.
//
// Most bins come from the stream ports and from the configuration seen on the APB bus (events
// from pfdaq_stream_checks). A few come from probes into the design, connected by the wrapper
// tb_pfdaq_assert.v: the state of the run state machine, the reason for a real-time drop, and
// the state in which an abort arrived. Those bins are marked "probe" in the table.

`timescale 1ns / 1ps

module pfdaq_stream_cover (
    input             ACLK,
    input             RSTN,
    input             TVALID,
    input             TREADY,
    // configuration of the run, from pfdaq_apb_snoop
    input      [15:0] run_payload,
    input      [31:0] run_frames,
    input      [31:0] run_gap,
    input      [31:0] run_rate,
    input             run_tlpf,
    input             run_rt,
    input      [1:0]  run_mode,
    input      [1:0]  run_tdn,
    input      [2:0]  run_fault_en,
    input             start_tgl,
    input             abort_tgl,
    input             fabort_tgl,
    // events from pfdaq_stream_checks
    input             pkt_open_i,
    input      [31:0] beats_in_run_i,
    input             ev_beat,
    input      [15:0] ev_wait,
    input             ev_first,
    input             ev_last,
    input             ev_odd,
    input      [31:0] ev_idle,
    input             ev_pkt_gap_v,
    input             ev_frame,
    input             ev_frame_inpkt,
    input             ev_frame_hi,
    input      [1:0]  ev_tdest,
    input             ev_seqgap_v,
    input      [31:0] ev_seqgap,
    // probes into the design (see tb_pfdaq_assert.v)
    input      [2:0]  p_run,
    input             p_abort_pulse,
    input             p_drop,
    input      [15:0] p_credits,
    input             p_abort_pend
);
    localparam NB = 89;
    integer c [0:NB-1];
    reg [8*28-1:0] gname [0:NB-1];
    reg [8*50-1:0] bname [0:NB-1];
    integer i;

    task def(input integer n, input [8*28-1:0] g, input [8*50-1:0] b);
        begin gname[n] = g; bname[n] = b; end
    endtask

    initial begin
        for (i = 0; i < NB; i = i + 1) c[i] = 0;
        def( 0, "backpressure (TREADY low)", "beat accepted after 0 clocks of waiting");
        def( 1, "backpressure (TREADY low)", "beat accepted after 1 clock of waiting");
        def( 2, "backpressure (TREADY low)", "beat accepted after 2 clocks of waiting");
        def( 3, "backpressure (TREADY low)", "beat accepted after 3 to 7 clocks of waiting");
        def( 4, "backpressure (TREADY low)", "beat accepted after 8+ clocks of waiting");
        def( 5, "stalled beat is", "the first beat of a packet");
        def( 6, "stalled beat is", "a middle beat of a packet");
        def( 7, "stalled beat is", "the TLAST beat");
        def( 8, "spacing", "frame starts inside a packet, no gap");
        def( 9, "spacing", "packet starts 0 clocks after the previous");
        def(10, "spacing", "packet starts 1 to 7 clocks after the previous");
        def(11, "spacing", "packet starts 8 to 99 clocks after the previous");
        def(12, "spacing", "packet starts 100+ clocks after the previous");
        def(13, "payload words per frame", "1 (minimum)");
        def(14, "payload words per frame", "2 to 8");
        def(15, "payload words per frame", "9 to 64");
        def(16, "payload words per frame", "65 to 500");
        def(17, "payload words per frame", "501 to 65534");
        def(18, "payload words per frame", "65535 (maximum)");
        def(19, "frames per run", "1");
        def(20, "frames per run", "2 to 8");
        def(21, "frames per run", "9 to 64");
        def(22, "frames per run", "65 or more");
        def(23, "4-byte last beat (TKEEP 0F)", "burst, one packet per run");
        def(24, "4-byte last beat (TKEEP 0F)", "burst, TLAST per frame");
        def(25, "4-byte last beat (TKEEP 0F)", "real-time");
        def(26, "kind x payload mode", "burst, counter");
        def(27, "kind x payload mode", "burst, PRBS");
        def(28, "kind x payload mode", "burst, FIR");
        def(29, "kind x payload mode", "TLAST per frame, counter");
        def(30, "kind x payload mode", "TLAST per frame, PRBS");
        def(31, "kind x payload mode", "TLAST per frame, FIR");
        def(32, "kind x payload mode", "real-time, counter");
        def(33, "kind x payload mode", "real-time, PRBS");
        def(34, "kind x payload mode", "real-time, FIR");
        def(35, "TDEST of accepted packets", "TDEST 0");
        def(36, "TDEST of accepted packets", "TDEST 1");
        def(37, "TDEST of accepted packets", "TDEST 2");
        def(38, "TDEST of accepted packets", "TDEST 3");
        def(39, "real-time destinations", "1 destination (TDEST_COUNT 1)");
        def(40, "real-time destinations", "2 destinations");
        def(41, "real-time destinations", "3 destinations");
        def(42, "real-time destinations", "4 destinations");
        def(43, "real-time RATE (clocks)", "2 (minimum)");
        def(44, "real-time RATE (clocks)", "3 to 19");
        def(45, "real-time RATE (clocks)", "20 to 99");
        def(46, "real-time RATE (clocks)", "100 or more");
        def(47, "slots dropped before a frame", "0 dropped");
        def(48, "slots dropped before a frame", "1 dropped");
        def(49, "slots dropped before a frame", "2 or 3 dropped");
        def(50, "slots dropped before a frame", "4 or more dropped");
        def(51, "real-time drop reason", "probe: no credit");
        def(52, "real-time drop reason", "probe: source busy sending a frame");
        def(53, "real-time drop reason", "probe: abort pending");
        def(54, "GAP_CYCLES (TLAST per frame)", "0");
        def(55, "GAP_CYCLES (TLAST per frame)", "1 to 7");
        def(56, "GAP_CYCLES (TLAST per frame)", "8 to 399");
        def(57, "GAP_CYCLES (TLAST per frame)", "400 or more");
        def(58, "fault x payload mode", "corrupt CRC, counter");
        def(59, "fault x payload mode", "corrupt CRC, PRBS");
        def(60, "fault x payload mode", "corrupt CRC, FIR");
        def(61, "fault x payload mode", "skip sequence number, counter");
        def(62, "fault x payload mode", "skip sequence number, PRBS");
        def(63, "fault x payload mode", "skip sequence number, FIR");
        def(64, "fault x payload mode", "flip payload bit, counter");
        def(65, "fault x payload mode", "flip payload bit, PRBS");
        def(66, "fault x payload mode", "flip payload bit, FIR");
        def(67, "CTRL.ABORT in run kind", "burst, no packet open");
        def(68, "CTRL.ABORT in run kind", "burst, packet open");
        def(69, "CTRL.ABORT in run kind", "TLAST per frame, no packet open");
        def(70, "CTRL.ABORT in run kind", "TLAST per frame, packet open");
        def(71, "CTRL.ABORT in run kind", "real-time, no packet open");
        def(72, "CTRL.ABORT in run kind", "real-time, packet open");
        def(73, "CTRL.ABORT when", "TVALID high and TREADY low (stalled)");
        def(74, "CTRL.ABORT when", "no beat of the run sent yet");
        def(75, "abort received in state", "probe: PREFILL (FIR filling)");
        def(76, "abort received in state", "probe: RTWAIT (real-time, waiting)");
        def(77, "abort received in state", "probe: RUN");
        def(78, "abort received in state", "probe: GAP");
        def(79, "abort received in state", "probe: FILL (zero fill)");
        def(80, "abort received in state", "probe: IDLE (ignored)");
        def(81, "FORCE_ABORT when", "TVALID high");
        def(82, "FORCE_ABORT when", "TVALID low");
        def(83, "state entered", "probe: PREFILL");
        def(84, "state entered", "probe: RTWAIT");
        def(85, "state entered", "probe: RUN");
        def(86, "state entered", "probe: GAP");
        def(87, "state entered", "probe: FILL");
        def(88, "frame start", "in the high word of a beat");
    end

    // totals that are not bins
    integer runs = 0, beats = 0, frames = 0, packets = 0;
    integer max_wait = 0, streak = 0, max_streak = 0;
    integer max_payload = 0, min_payload = 70000;
    integer max_frames = 0, min_frames = 1000000, max_rate = 0, min_rate = 1000000;
    reg        started = 0, start_q, abort_q, fabort_q, armed = 0;
    reg [2:0]  prev_run = 3'd0;
    wire [1:0] kind = run_rt ? 2'd2 : (run_tlpf ? 2'd1 : 2'd0);
    integer    m, f;

    task hit(input integer n); begin c[n] = c[n] + 1; end endtask

    always @(posedge ACLK) begin
        if (RSTN !== 1'b1) begin
            armed = 0;
        end else if (!armed) begin
            armed = 1; start_q = start_tgl; abort_q = abort_tgl; fabort_q = fabort_tgl; prev_run = p_run;
        end else begin
            // ---- run start: the configuration the scenario chose ----
            if (start_tgl !== start_q) begin
                start_q = start_tgl;
                started = 1; runs = runs + 1; streak = 0;
                m = run_mode;
                if (run_payload == 16'd1) hit(13);
                else if (run_payload <= 16'd8) hit(14);
                else if (run_payload <= 16'd64) hit(15);
                else if (run_payload <= 16'd500) hit(16);
                else if (run_payload != 16'hFFFF) hit(17);
                else hit(18);
                if (run_payload > max_payload) max_payload = run_payload;
                if (run_payload < min_payload) min_payload = run_payload;
                if (run_frames > max_frames) max_frames = run_frames;
                if (run_frames < min_frames) min_frames = run_frames;
                if (run_frames == 32'd1) hit(19);
                else if (run_frames <= 32'd8) hit(20);
                else if (run_frames <= 32'd64) hit(21);
                else hit(22);
                hit(26 + kind * 3 + m);
                if (run_rt) begin
                    if (run_rate > max_rate) max_rate = run_rate;
                    if (run_rate < min_rate) min_rate = run_rate;
                    hit(39 + run_tdn);
                    if (run_rate == 32'd2) hit(43);
                    else if (run_rate < 32'd20) hit(44);
                    else if (run_rate < 32'd100) hit(45);
                    else hit(46);
                end
                if (run_tlpf && !run_rt) begin
                    if (run_gap == 32'd0) hit(54);
                    else if (run_gap < 32'd8) hit(55);
                    else if (run_gap < 32'd400) hit(56);
                    else hit(57);
                end
                for (f = 0; f < 3; f = f + 1)
                    if (run_fault_en[f]) hit(58 + f * 3 + m);
            end
            // ---- abort requests: where the run was when software asked ----
            if (abort_tgl !== abort_q) begin
                abort_q = abort_tgl;
                hit(67 + kind * 2 + (pkt_open_i ? 1 : 0));
                if (TVALID === 1'b1 && TREADY !== 1'b1) hit(73);
                if (beats_in_run_i == 0) hit(74);
            end
            if (fabort_tgl !== fabort_q) begin
                fabort_q = fabort_tgl;
                if (TVALID === 1'b1) hit(81); else hit(82);
            end
            // ---- probes ----
            if (p_abort_pulse) begin
                case (p_run)
                    3'd1: hit(75);
                    3'd4: hit(76);
                    3'd2: hit(77);
                    3'd3: hit(78);
                    3'd5: hit(79);
                    default: hit(80);
                endcase
            end
            if (p_run !== prev_run) begin
                case (p_run)
                    3'd1: hit(83);
                    3'd4: hit(84);
                    3'd2: hit(85);
                    3'd3: hit(86);
                    3'd5: hit(87);
                    default: ;
                endcase
                prev_run = p_run;
            end
            if (p_drop) begin
                if (p_run == 3'd2) hit(52);
                else if (p_abort_pend) hit(53);
                else if (p_credits == 16'd0) hit(51);
            end
            // ---- stream events (one clock late, see pfdaq_stream_checks) ----
            if (ev_beat) begin
                beats = beats + 1;
                if (ev_wait == 0) hit(0);
                else if (ev_wait == 1) hit(1);
                else if (ev_wait == 2) hit(2);
                else if (ev_wait < 8) hit(3);
                else hit(4);
                if (ev_wait > max_wait) max_wait = ev_wait;
                if (ev_wait != 0) begin
                    if (ev_first) hit(5);
                    if (ev_last) hit(7);
                    if (!ev_first && !ev_last) hit(6);
                end
                if (ev_first) begin
                    packets = packets + 1;
                    hit(35 + ev_tdest);
                    if (ev_pkt_gap_v) begin
                        if (ev_idle == 0) hit(9);
                        else if (ev_idle < 8) hit(10);
                        else if (ev_idle < 100) hit(11);
                        else hit(12);
                    end
                end
                if (ev_odd) hit(23 + kind);
            end
            if (ev_beat) streak = streak + 1; else streak = 0;
            if (streak > max_streak) max_streak = streak;
            if (ev_frame) begin
                frames = frames + 1;
                if (ev_frame_inpkt) hit(8);
                if (ev_frame_hi) hit(88);
            end
            if (ev_seqgap_v) begin
                if (ev_seqgap == 0) hit(47);
                else if (ev_seqgap == 1) hit(48);
                else if (ev_seqgap <= 3) hit(49);
                else hit(50);
            end
        end
    end

    function void report;
        integer n, hits;
        begin
            hits = 0;
            $display("COV functional coverage of the stream source");
            $display("COV %-3s %-28s %-48s %6s", "bin", "group", "what is counted", "count");
            for (n = 0; n < NB; n = n + 1) begin
                if (c[n] > 0) hits = hits + 1;
                if (c[n] == 0) $display("COV %3d %-28s %-48s %6d  NOT HIT", n, gname[n], bname[n], c[n]);
                else           $display("COV %3d %-28s %-48s %6d", n, gname[n], bname[n], c[n]);
            end
            $display("COV bins hit %0d of %0d", hits, NB);
            $display("COV runs started %0d, packets %0d, frame headers %0d, accepted beats %0d", runs, packets, frames, beats);
            $display("COV longest wait of one beat %0d clocks, longest streak of beats on consecutive clocks %0d", max_wait, max_streak);
            $display("COV payload words per frame: smallest %0d, largest %0d", min_payload, max_payload);
            $display("COV frames per run: smallest %0d, largest %0d", min_frames, max_frames);
            $display("COV real-time RATE: smallest %0d, largest %0d", min_rate, max_rate);
        end
    endfunction
endmodule
