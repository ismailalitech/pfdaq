// Self-checking test of the two blocks wired as in the SmartDesign: master -> registers -> stream sink.
`timescale 1ns / 1ps
module tb_pfdaq_demo_top;
    reg PCLK = 0, PRESETN = 0;
    wire PSEL, PENABLE, PWRITE, PREADY, PSLVERR, TVALID, TLAST;
    wire [31:0] PADDR, PWDATA, PRDATA, TDATA;
    integer beats = 0, errors = 0;

    pfdaq_demo_master m (.PCLK(PCLK), .PRESETN(PRESETN), .PSEL(PSEL), .PENABLE(PENABLE), .PWRITE(PWRITE),
        .PADDR(PADDR), .PWDATA(PWDATA), .PRDATA(PRDATA), .PREADY(PREADY), .PSLVERR(PSLVERR));
    pfdaq_demo_regs r (.PCLK(PCLK), .PRESETN(PRESETN), .PSEL(PSEL), .PENABLE(PENABLE), .PWRITE(PWRITE),
        .PADDR(PADDR), .PWDATA(PWDATA), .PRDATA(PRDATA), .PREADY(PREADY), .PSLVERR(PSLVERR),
        .TDATA(TDATA), .TVALID(TVALID), .TREADY(1'b1), .TLAST(TLAST));

    always #5 PCLK = ~PCLK;
    always @(posedge PCLK) if (TVALID) begin
        if (TDATA !== beats) begin errors = errors + 1; $display("FAIL: beat %0d carries %0d", beats, TDATA); end
        if (TLAST !== (beats == 7)) begin errors = errors + 1; $display("FAIL: TLAST wrong on beat %0d", beats); end
        beats = beats + 1;
    end
    initial begin
        #23 PRESETN = 1;
        #2000;
        if (beats !== 8) begin errors = errors + 1; $display("FAIL: %0d beats, expected 8", beats); end
        if (errors == 0) $display("PASS: the master started a burst of 8 words, TLAST on the eighth");
        else $display("FAILED: %0d errors", errors);
        $finish;
    end
endmodule
