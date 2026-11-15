// Testbench for the latency unit inside pfdaq_core. A single APB master plays "software":
// it acknowledges each interrupt after a random delay and reads LAT_LAST back. Every interrupt
// rise, acknowledge strobe and register read is logged to sim/lat_trace.txt for check_latency.py,
// which recomputes every latency and all statistics independently.
`timescale 1ns / 1ps
module tb_pfdaq_latency;
    reg PCLK = 0, ACLK = 0, RSTN = 0;
    reg PSEL = 0, PENABLE = 0, PWRITE = 0;
    reg [31:0] PADDR = 0, PWDATA = 0;
    wire [31:0] PRDATA; wire [63:0] TDATA; wire [7:0] TKEEP, TSTRB, TID; wire [1:0] TDEST;
    wire TLAST, TVALID, PREADY, PSLVERR, IRQ0, IRQ1;
    integer f, seed = 7, cyc = 0, phase = 0;
    integer maxdelay [0:1]; integer ack_at [0:1]; reg armed [0:1]; reg prev [0:1];
    reg [31:0] rd;
    wire [1:0] irq = {IRQ1, IRQ0};

    always #10 PCLK = ~PCLK;   // 50 MHz
    always #4  ACLK = ~ACLK;

    pfdaq_core dut (.ACLK(ACLK), .RSTN(RSTN), .TREADY(1'b1), .TDEST(TDEST), .TID(TID), .TKEEP(TKEEP),
        .TLAST(TLAST), .TVALID(TVALID), .TSTRB(TSTRB), .TDATA(TDATA), .PCLK(PCLK), .PRESETN(RSTN),
        .PENABLE(PENABLE), .PSEL(PSEL), .PADDR(PADDR), .PWRITE(PWRITE), .PWDATA(PWDATA),
        .PRDATA(PRDATA), .PREADY(PREADY), .PSLVERR(PSLVERR), .IRQ_LAT0(IRQ0), .IRQ_LAT1(IRQ1));

    // observer: logs what a logic analyser on the bus and the interrupt lines would see
    integer c;
    always @(posedge PCLK) begin
        cyc <= cyc + 1;
        for (c = 0; c < 2; c = c + 1) begin
            if (irq[c] && !prev[c]) $fwrite(f, "I %0d %0d %0d\n", phase, c, cyc);
            prev[c] <= irq[c];
        end
        if (PSEL && PENABLE && PWRITE && PADDR[7] && (PADDR[5:0] == 6'h0C))
            $fwrite(f, "K %0d %0d %0d %0d\n", phase, PADDR[6], cyc, irq[PADDR[6]]);
    end

    task apb_wr(input [31:0] a, input [31:0] d); begin
        @(posedge PCLK); PSEL <= 1; PWRITE <= 1; PADDR <= 32'h40000F00 + a; PWDATA <= d; PENABLE <= 0;
        @(posedge PCLK); PENABLE <= 1;
        @(posedge PCLK); PSEL <= 0; PENABLE <= 0; PWRITE <= 0;
    end endtask
    task apb_rd(input [31:0] a); begin
        @(posedge PCLK); PSEL <= 1; PWRITE <= 0; PADDR <= 32'h40000F00 + a; PENABLE <= 0;
        @(posedge PCLK); PENABLE <= 1;
        @(posedge PCLK); rd = PRDATA; PSEL <= 0; PENABLE <= 0;
        $fwrite(f, "R %0d %02x %08x\n", phase, a[7:0], rd);
    end endtask

    // a CLEAR takes 64 cycles in the RAM version; events are held off meanwhile
    task wait_clear(input integer ch); begin
        rd = 32'h4;
        while (rd[2]) apb_rd(32'h90 + ch * 32'h40);
    end endtask

    // "software": serve interrupts until both channels are idle and disabled
    task serve(input integer guard_cycles);
        integer ch, start, busy, both;
        begin
            start = cyc; busy = 1;
            while (busy && (cyc - start < guard_cycles)) begin
                @(posedge PCLK);
                for (ch = 0; ch < 2; ch = ch + 1) begin
                    if (irq[ch] && !armed[ch]) begin
                        armed[ch] = 1;
                        ack_at[ch] = cyc + (($random(seed) & 32'h7fffffff) % (maxdelay[ch] + 1));
                    end
                    if (armed[ch] && cyc >= ack_at[ch]) begin
                        apb_wr(32'h8C + ch * 32'h40, 32'h1);
                        armed[ch] = 0;
                        apb_rd(32'hA0 + ch * 32'h40);      // LAT_LAST
                    end
                end
                both = 0;
                apb_rd(32'h90); both = both | rd[1:0];
                apb_rd(32'hD0); both = both | rd[1:0];
                busy = (both != 0) || armed[0] || armed[1];
            end
            if (busy) $fwrite(f, "TIMEOUT %0d\n", phase);
        end
    endtask

    task dump(input integer ch);
        integer b, base;
        begin
            base = 32'h80 + ch * 32'h40;
            apb_rd(base + 32'h14); apb_rd(base + 32'h18); apb_rd(base + 32'h1C); apb_rd(base + 32'h24);
            apb_rd(base + 32'h28); apb_rd(base + 32'h2C); apb_rd(base + 32'h30); apb_rd(base + 32'h3C);
            for (b = 0; b < 64; b = b + 1) begin
                apb_wr(base + 32'h34, b);
                @(posedge PCLK); PSEL <= 1; PWRITE <= 0; PADDR <= 32'h40000F00 + base + 32'h38; PENABLE <= 0;
                @(posedge PCLK); PENABLE <= 1;
                @(posedge PCLK); rd = PRDATA; PSEL <= 0; PENABLE <= 0;
                $fwrite(f, "H %0d %0d %0d %0d\n", phase, ch, b, rd);
            end
        end
    endtask

    initial begin
        f = $fopen("sim/lat_trace.txt", "w");
        armed[0] = 0; armed[1] = 0; prev[0] = 0; prev[1] = 0;
        repeat (5) @(posedge PCLK); RSTN = 1; repeat (5) @(posedge PCLK);
        wait_clear(0); wait_clear(1);                               // power-on clear sweep
        phase = 0; apb_rd(32'h00); apb_rd(32'h04);                 // stream source still reachable

        // phase 1: both channels at once, acknowledge always in time
        phase = 1; maxdelay[0] = 100; maxdelay[1] = 40;
        apb_wr(32'h84, 32'd400);  apb_wr(32'h88, 32'd60);
        apb_wr(32'hC4, 32'd331);  apb_wr(32'hC8, 32'd90);
        apb_wr(32'h80, 32'h0000_0001);                              // ch0: no jitter
        apb_wr(32'hC0, 32'h003F_0001);                              // ch1: jitter 0..63 cycles
        serve(200000); dump(0); dump(1);

        // phase 2: channel 0 acknowledged too slowly -> overruns; statistics cleared first
        phase = 2; maxdelay[0] = 300; maxdelay[1] = 0;
        apb_wr(32'h80, 32'h4); apb_wr(32'hC0, 32'h4); wait_clear(0); wait_clear(1);
        apb_wr(32'h84, 32'd120); apb_wr(32'h88, 32'd25);
        apb_wr(32'h80, 32'h1);
        serve(200000); dump(0);

        // phase 3: one-shot events with long latencies (upper histogram bins), then a spurious acknowledge
        phase = 3; maxdelay[1] = 40000;
        apb_wr(32'hC0, 32'h4); wait_clear(1);
        apb_wr(32'hC0, 32'h2); serve(100000);
        apb_wr(32'hC0, 32'h2); serve(100000);
        apb_wr(32'hCC, 32'h1);                                      // nothing pending
        dump(1);
        $fwrite(f, "END\n"); $fclose(f); $finish;
    end
    initial begin #400000000; $display("GLOBAL TIMEOUT"); $finish; end
endmodule
