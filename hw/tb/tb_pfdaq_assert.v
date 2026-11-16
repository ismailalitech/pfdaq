// tb_pfdaq_assert - second top level for the stream source: runs the existing testbench
// (tb_pfdaq_stream_src.v, unchanged, instantiated as "tb") and watches it with the protocol
// checks and the coverage counters. The existing testbench is reached only by hierarchical
// references to its signals; nothing in it is modified, and its trace is the same.
//
// Build and run: make -C hw sim-assert
`timescale 1ns / 1ps
module tb_pfdaq_assert;
    tb_pfdaq_stream_src tb ();

    // stream and APB signals of the existing testbench
    wire        aclk = tb.ACLK, rstn = tb.RSTN, pclk = tb.PCLK;
    wire        tvalid = tb.TVALID, tready = tb.TREADY, tlast = tb.TLAST;
    wire [63:0] tdata = tb.TDATA;
    wire [7:0]  tkeep = tb.TKEEP, tstrb = tb.TSTRB, tid = tb.TID;
    wire [1:0]  tdest = tb.TDEST;
    wire        psel = tb.PSEL, penable = tb.PENABLE, pwrite = tb.PWRITE;
    wire [31:0] paddr = tb.PADDR, pwdata = tb.PWDATA, prdata = tb.PRDATA;
    wire [31:0] tag = tb.scenario;

    // probes into the design, used by the coverage only
    wire [2:0]  p_run         = tb.dut.run;
    wire        p_abort_pulse = tb.dut.abort_pulse_a;
    wire        p_drop        = tb.dut.drop_now;
    wire [15:0] p_credits     = tb.dut.credits;
    wire        p_abort_pend  = tb.dut.abort_pend;

    wire [15:0] run_payload, run_fault_idx;
    wire [31:0] run_frames, run_gap, run_rate;
    wire        run_tlpf, run_rt, start_tgl, abort_tgl, fabort_tgl, underrun_tgl;
    wire [1:0]  run_mode, run_tdn;
    wire [2:0]  run_fault_en;

    pfdaq_apb_snoop snoop (.PCLK(pclk), .PSEL(psel), .PENABLE(penable), .PWRITE(pwrite),
        .PADDR(paddr), .PWDATA(pwdata), .PRDATA(prdata),
        .run_payload(run_payload), .run_frames(run_frames), .run_gap(run_gap), .run_rate(run_rate),
        .run_tlpf(run_tlpf), .run_rt(run_rt), .run_mode(run_mode), .run_tdn(run_tdn),
        .run_fault_en(run_fault_en), .run_fault_idx(run_fault_idx),
        .start_tgl(start_tgl), .abort_tgl(abort_tgl), .fabort_tgl(fabort_tgl), .underrun_tgl(underrun_tgl));

    wire        pkt_open_o, ev_beat, ev_first, ev_last, ev_odd, ev_pkt_gap_v, ev_frame, ev_frame_inpkt,
                ev_frame_hi, ev_seqgap_v, ev_forced_stop;
    wire [31:0] beats_in_run_o, ev_idle, ev_seqgap;
    wire [15:0] ev_wait;
    wire [1:0]  ev_tdest;

    pfdaq_stream_checks chk (.ACLK(aclk), .RSTN(rstn), .TVALID(tvalid), .TREADY(tready), .TDATA(tdata),
        .TKEEP(tkeep), .TSTRB(tstrb), .TLAST(tlast), .TDEST(tdest), .TID(tid), .tag(tag),
        .run_payload(run_payload), .run_frames(run_frames), .run_tlpf(run_tlpf), .run_rt(run_rt),
        .run_tdn(run_tdn), .run_fault_en(run_fault_en),
        .start_tgl(start_tgl), .abort_tgl(abort_tgl), .fabort_tgl(fabort_tgl), .underrun_tgl(underrun_tgl),
        .pkt_open_o(pkt_open_o), .beats_in_run_o(beats_in_run_o),
        .ev_beat(ev_beat), .ev_wait(ev_wait), .ev_first(ev_first), .ev_last(ev_last), .ev_odd(ev_odd),
        .ev_idle(ev_idle), .ev_pkt_gap_v(ev_pkt_gap_v), .ev_frame(ev_frame), .ev_frame_inpkt(ev_frame_inpkt),
        .ev_frame_hi(ev_frame_hi), .ev_tdest(ev_tdest), .ev_seqgap_v(ev_seqgap_v), .ev_seqgap(ev_seqgap),
        .ev_forced_stop(ev_forced_stop));

    pfdaq_stream_cover cov (.ACLK(aclk), .RSTN(rstn), .TVALID(tvalid), .TREADY(tready),
        .run_payload(run_payload), .run_frames(run_frames), .run_gap(run_gap), .run_rate(run_rate),
        .run_tlpf(run_tlpf), .run_rt(run_rt), .run_mode(run_mode), .run_tdn(run_tdn),
        .run_fault_en(run_fault_en), .start_tgl(start_tgl), .abort_tgl(abort_tgl), .fabort_tgl(fabort_tgl),
        .pkt_open_i(pkt_open_o), .beats_in_run_i(beats_in_run_o),
        .ev_beat(ev_beat), .ev_wait(ev_wait), .ev_first(ev_first), .ev_last(ev_last), .ev_odd(ev_odd),
        .ev_idle(ev_idle), .ev_pkt_gap_v(ev_pkt_gap_v), .ev_frame(ev_frame), .ev_frame_inpkt(ev_frame_inpkt),
        .ev_frame_hi(ev_frame_hi), .ev_tdest(ev_tdest), .ev_seqgap_v(ev_seqgap_v), .ev_seqgap(ev_seqgap),
        .p_run(p_run), .p_abort_pulse(p_abort_pulse), .p_drop(p_drop), .p_credits(p_credits),
        .p_abort_pend(p_abort_pend));

    // The existing testbench ends the run with $finish; the final block prints both reports.
    final begin
        chk.report();
        cov.report();
        if (chk.total_viol == 0) $display("ASSERT RESULT: PASS");
        else                     $display("ASSERT RESULT: FAIL (%0d violations)", chk.total_viol);
    end
endmodule
