// Testbench for pfdaq_stream_src v0.5: drives APB, applies random TREADY backpressure, logs
// every accepted stream word (with TLAST and TDEST) and the register readbacks to
// sim/trace.txt. check_trace.py rebuilds every frame from an independent model.
`timescale 1ns / 1ps
module tb_pfdaq_stream_src;
    reg ACLK = 0, PCLK = 0, RSTN = 0, PRESETN = 0;
    reg TREADY = 0;
    reg PSEL = 0, PENABLE = 0, PWRITE = 0;
    reg [31:0] PADDR = 0, PWDATA = 0;
    wire [31:0] PRDATA; wire [63:0] TDATA;
    wire [1:0] TDEST; wire [7:0] TID; wire [7:0] TKEEP, TSTRB;
    wire TLAST, TVALID, PREADY, PSLVERR;
    integer f, ready_pct = 100, seed = 1, scenario = 0, k;
    reg [31:0] rd;
    reg abort_seen = 0;

    always #4  ACLK = ~ACLK;   // 125 MHz
    always #10 PCLK = ~PCLK;   // 50 MHz

    pfdaq_stream_src #(.VERSION_VALUE(32'h00050000)) dut (.ACLK(ACLK), .RSTN(RSTN), .TREADY(TREADY),
        .TDEST(TDEST), .TID(TID), .TKEEP(TKEEP), .TLAST(TLAST), .TVALID(TVALID), .TSTRB(TSTRB), .TDATA(TDATA),
        .PCLK(PCLK), .PRESETN(PRESETN), .PENABLE(PENABLE), .PSEL(PSEL), .PADDR(PADDR),
        .PWRITE(PWRITE), .PWDATA(PWDATA), .PRDATA(PRDATA), .PREADY(PREADY), .PSLVERR(PSLVERR));

    always @(posedge ACLK) TREADY <= (($random(seed) & 32'h7fffffff) % 100) < ready_pct;

    // AXI4-Stream rules: data stable while stalled; no TVALID gap inside a packet (DMA requirement);
    // byte lanes all valid except a 4-byte last beat
    reg        pv = 0; reg [63:0] pd = 0; reg pl = 0; reg in_pkt = 0; reg [1:0] pdst = 0;
    always @(posedge ACLK) begin
        if (pv && !abort_seen && (!TVALID || TDATA !== pd || TLAST !== pl || TDEST !== pdst))
            $fwrite(f, "PROTOCOL_ERROR %0t\n", $time);
        pv <= TVALID & ~TREADY; pd <= TDATA; pl <= TLAST; pdst <= TDEST;
        if (in_pkt && !TVALID && !abort_seen) $fwrite(f, "VALID_GAP %0t\n", $time);
        if (TVALID && TREADY) in_pkt <= ~TLAST;
        if (!RSTN || abort_seen) in_pkt <= 0;
        if (TVALID && (TKEEP !== TSTRB || !(TKEEP === 8'hFF || (TLAST && TKEEP === 8'h0F))))
            $fwrite(f, "KEEP_ERROR %0t\n", $time);
        if (TVALID && TREADY) begin
            if (TKEEP[7])
                $fwrite(f, "B %0d %08x 0 %0d %0d\nB %0d %08x %0d %0d %0d\n", scenario, TDATA[31:0], $time, TDEST,
                        scenario, TDATA[63:32], TLAST, $time, TDEST);
            else
                $fwrite(f, "B %0d %08x %0d %0d %0d\n", scenario, TDATA[31:0], TLAST, $time, TDEST);
        end
    end

    task apb_wr(input [31:0] a, input [31:0] d); begin
        @(posedge PCLK); PSEL <= 1; PWRITE <= 1; PADDR <= a; PWDATA <= d; PENABLE <= 0;
        @(posedge PCLK); PENABLE <= 1;
        @(posedge PCLK); PSEL <= 0; PENABLE <= 0; PWRITE <= 0;
    end endtask
    task apb_rd_q(input [31:0] a); begin
        @(posedge PCLK); PSEL <= 1; PWRITE <= 0; PADDR <= a; PENABLE <= 0;
        @(posedge PCLK); PENABLE <= 1;
        @(posedge PCLK); rd = PRDATA; PSEL <= 0; PENABLE <= 0;
    end endtask
    task apb_rd(input [31:0] a); begin
        apb_rd_q(a);
        $fwrite(f, "R %0d %02x %08x\n", scenario, a[7:0], rd);
    end endtask
    task snapshot(input pre); begin
        apb_wr(32'h08, 32'h2); repeat (6) @(posedge PCLK);
        apb_rd_q(32'h20); $fwrite(f, "%s %0d 20 %08x\n", pre ? "P" : "R", scenario, rd);
        apb_rd_q(32'h24); $fwrite(f, "%s %0d 24 %08x\n", pre ? "P" : "R", scenario, rd);
        apb_rd_q(32'h30); $fwrite(f, "%s %0d 30 %08x\n", pre ? "P" : "R", scenario, rd);
        apb_rd_q(32'h40); $fwrite(f, "%s %0d 40 %08x\n", pre ? "P" : "R", scenario, rd);
        apb_rd_q(32'h3C); $fwrite(f, "%s %0d 3c %08x\n", pre ? "P" : "R", scenario, rd);
    end endtask

    // one run. mode: payload 0/1/2; rt: real-time; tdn: destinations-1; rate: real-time interval;
    // credits: written before START (real-time); abort_after: PCLK cycles after START, 0 = none
    task run(input integer sc, input [15:0] words, input [31:0] frames, input [31:0] gap,
             input tlpf, input [31:0] flt, input integer rdy, input [1:0] mode, input rt,
             input [1:0] tdn, input [31:0] rate, input [15:0] credits, input [15:0] step,
             input integer abort_after);
        integer guard;
        begin
            scenario = sc; ready_pct = rdy;
            $fwrite(f, "S %0d %0d %0d %0d %0d %08x %0d %0d %0d %0d %0d %0d %0d %0d\n",
                    sc, words, frames, gap, tlpf, flt, rdy, mode, rt, tdn, rate, credits, step, abort_after);
            apb_wr(32'h0C, words); apb_wr(32'h10, frames); apb_wr(32'h14, gap); apb_wr(32'h18, flt);
            apb_wr(32'h34, {22'd0, tdn, 3'd0, rt, 2'd0, mode}); apb_wr(32'h38, rate); apb_wr(32'h44, step);
            apb_wr(32'h3C, {1'b1, 15'd0, credits});
            snapshot(1);
            apb_wr(32'h08, {23'd0, tlpf, 8'h01});
            if (abort_after > 0) begin
                repeat (abort_after) @(posedge PCLK);
                apb_wr(32'h08, {23'd0, tlpf, 8'h10});
            end
            guard = 0; rd = 0;
            while (!rd[1] && guard < 400000) begin apb_rd_q(32'h1C); guard = guard + 1; end
            if (guard >= 400000) $fwrite(f, "TIMEOUT %0d\n", sc);
            apb_rd(32'h1C);
            snapshot(0);
            repeat (20) @(posedge ACLK);
        end
    endtask

    initial begin
        f = $fopen("sim/trace.txt", "w");
        repeat (5) @(posedge PCLK); RSTN = 1; PRESETN = 1; repeat (5) @(posedge PCLK);
        apb_rd(32'h00); apb_rd(32'h04);
        // FIR coefficients (large, mixed signs, so the 32-bit sum wraps)
        apb_wr(32'h48, 16'd12000); apb_wr(32'h4C, -16'sd15000); apb_wr(32'h50, 16'd20000); apb_wr(32'h54, 16'd32767);
        apb_wr(32'h58, -16'sd32768); apb_wr(32'h5C, 16'd20000); apb_wr(32'h60, -16'sd15000); apb_wr(32'h64, 16'd12000);
        for (k = 0; k < 8; k = k + 1) apb_rd(32'h48 + 4 * k);
        //   sc  words frames gap tlpf fault          rdy mode rt tdn rate credits step  abort
        run( 1,  8,    5,    0,  0,  32'h0000_0000, 100, 0,   0, 0,  2,   0,      0,    0); // plain burst
        run( 2,  13,   20,   7,  1,  32'h0000_0000, 30,  0,   0, 0,  2,   0,      0,    0); // per-frame TLAST, gap
        run( 3,  8,    6,    2,  0,  32'h0002_0001, 70,  0,   0, 0,  2,   0,      0,    0); // CRC fault frame 2
        run( 4,  8,    6,    2,  0,  32'h0001_0002, 70,  0,   0, 0,  2,   0,      0,    0); // seq skip before frame 1
        run( 5,  8,    4,    0,  1,  32'h0000_0004, 50,  0,   0, 0,  2,   0,      0,    0); // payload flip frame 0
        run( 6,  1,    3,    0,  0,  32'h0000_0002, 90,  0,   0, 0,  2,   0,      0,    0); // minimum payload
        run( 7,  500,  40,   0,  0,  32'h0000_0000, 95,  0,   0, 0,  2,   0,      0,    0); // 80.6 kB burst
        run( 8,  9,    7,    0,  0,  32'h0000_0000, 60,  0,   0, 0,  2,   0,      0,    0); // odd word count
        run(10,  20,   6,    0,  0,  32'h0000_0000, 70,  1,   0, 0,  2,   0,      0,    0); // PRBS
        run(11,  33,   5,    0,  0,  32'h0000_0000, 60,  2,   0, 0,  2,   0,      1000, 0); // FIR, backpressure
        run(12,  500,  3,    0,  0,  32'h0000_0000, 100, 2,   0, 0,  2,   0,      77,   0); // FIR at full rate
        run(13,  40,   30,   0,  0,  32'h0000_0000, 100, 0,   1, 3,  200, 12,     0,    0); // real-time, credit drops
        run(14,  40,   20,   0,  0,  32'h0000_0000, 100, 2,   1, 1,  20,  100,    321,  0); // real-time, busy drops, FIR
        run(15,  20,   12,   0,  0,  32'h0001_0004, 80,  1,   1, 2,  60,  100,    0,    0); // real-time PRBS, fault
        run(16,  100,  50,   0,  0,  32'h0000_0000, 90,  0,   0, 0,  2,   0,      0,    300); // graceful abort mid-burst
        run(20,  33,   40,   0,  0,  32'h0000_0000, 70,  2,   0, 0,  2,   0,      555,  217); // abort mid-burst, FIR, odd frame length
        run(21,  8,    30,   0,  0,  32'h0000_0000, 100, 0,   0, 0,  2,   0,      0,    1);   // abort right after START
        run(17,  10,   20,   400,1,  32'h0000_0000, 100, 1,   0, 0,  2,   0,      0,    150); // abort between packets
        run(18,  30,   40,   0,  0,  32'h0000_0000, 100, 0,   1, 3,  100, 100,    0,    200); // abort in real-time
        // FORCE_ABORT into a stalled sink, then a normal run still works
        scenario = 99; ready_pct = 0;
        apb_wr(32'h34, 0); apb_wr(32'h0C, 32'd100); apb_wr(32'h10, 32'd50); apb_wr(32'h08, 32'h1);
        repeat (40) @(posedge PCLK); apb_rd(32'h1C);
        apb_wr(32'h08, 32'h20); abort_seen = 1; repeat (10) @(posedge PCLK); abort_seen = 0;
        apb_rd(32'h1C);
        run(19,  8,    5,    3,  1,  32'h0000_0000, 80,  0,   0, 0,  2,   0,      0,    0);
        apb_wr(32'h08, 32'h2); repeat (10) @(posedge PCLK); apb_rd(32'h2C);
        $fwrite(f, "END\n"); $fclose(f); $finish;
    end
    initial begin #4000000000; $display("GLOBAL TIMEOUT"); $finish; end
endmodule
