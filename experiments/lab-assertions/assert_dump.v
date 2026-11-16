// Second top level for waveforms: dumps a few stream signals and the rule number the checker reports,
// in a window of simulation time. Compile it together with tb_pfdaq_assert.v, with -s tb_pfdaq_assert -s assert_dump.
// The window start and length (ns) are plusargs: +T0=4380 +LEN=80.
`timescale 1ns / 1ps
module assert_dump;
    integer t0 = 4380, len = 80;
    initial begin
        if (!$value$plusargs("T0=%d", t0)) t0 = 4380;
        if (!$value$plusargs("LEN=%d", len)) len = 80;
        $dumpfile("assert.vcd");
        $dumpvars(0, tb_pfdaq_assert.aclk, tb_pfdaq_assert.tready, tb_pfdaq_assert.tvalid,
                  tb_pfdaq_assert.tdata, tb_pfdaq_assert.tlast, tb_pfdaq_assert.chk.v_now);
        $dumpoff;
        #(t0) $dumpon;
        #(len) $dumpoff;
    end
endmodule
