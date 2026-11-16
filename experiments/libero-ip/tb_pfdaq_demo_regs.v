// Self-checking test of pfdaq_demo_regs: register access and one burst with a stalling receiver.
`timescale 1ns / 1ps
module tb_pfdaq_demo_regs;
    reg PCLK = 0, PRESETN = 0, PSEL = 0, PENABLE = 0, PWRITE = 0, TREADY = 0;
    reg [31:0] PADDR = 0; reg [31:0] PWDATA = 0;
    wire [31:0] PRDATA, TDATA; wire PREADY, PSLVERR, TVALID, TLAST;
    integer errors = 0, beats = 0, i;
    reg [31:0] v;

    pfdaq_demo_regs dut (.PCLK(PCLK), .PRESETN(PRESETN), .PSEL(PSEL), .PENABLE(PENABLE), .PWRITE(PWRITE),
        .PADDR(PADDR), .PWDATA(PWDATA), .PRDATA(PRDATA), .PREADY(PREADY), .PSLVERR(PSLVERR),
        .TDATA(TDATA), .TVALID(TVALID), .TREADY(TREADY), .TLAST(TLAST));

    always #5 PCLK = ~PCLK;

    task apb_write(input [7:0] a, input [31:0] d);
        begin
            @(posedge PCLK); #1 PSEL = 1; PWRITE = 1; PADDR = a; PWDATA = d; PENABLE = 0;
            @(posedge PCLK); #1 PENABLE = 1;
            @(posedge PCLK); #1 PSEL = 0; PENABLE = 0; PWRITE = 0;
        end
    endtask
    task apb_read(input [7:0] a, output [31:0] d);
        begin
            @(posedge PCLK); #1 PSEL = 1; PWRITE = 0; PADDR = a; PENABLE = 0;
            @(posedge PCLK); #1 PENABLE = 1;
            @(posedge PCLK); d = PRDATA; #1 PSEL = 0; PENABLE = 0;
        end
    endtask
    task check(input [255:0] what, input [31:0] got, input [31:0] want);
        begin
            if (got !== want) begin errors = errors + 1; $display("FAIL %0s: got %h want %h", what, got, want); end
        end
    endtask

    // the receiver accepts a word on two clocks out of three
    always @(posedge PCLK) begin
        TREADY <= ($random & 3) != 0;
        if (TVALID && TREADY) begin
            check("TDATA", TDATA, beats);
            check("TLAST", TLAST, (beats == 5) ? 1 : 0);
            beats = beats + 1;
        end
    end

    initial begin
        #23 PRESETN = 1;
        apb_read(8'h04, v);  check("LEN reset", v, 4);
        apb_write(8'h0C, 32'hCAFEF00D); apb_read(8'h0C, v); check("SCRATCH", v, 32'hCAFEF00D);
        apb_write(8'h04, 6);            apb_read(8'h04, v); check("LEN", v, 6);
        apb_write(8'h00, 1);
        for (i = 0; i < 40; i = i + 1) @(posedge PCLK);
        apb_read(8'h08, v);  check("STATUS done", v, 2);
        apb_read(8'h10, v);  check("SENT", v, 6);
        check("beats", beats, 6);
        if (errors == 0) $display("PASS: 6 words, TLAST on the sixth, registers read back");
        else $display("FAILED: %0d errors", errors);
        $finish;
    end
endmodule
