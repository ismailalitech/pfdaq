// pfdaq_latency - fabric latency unit: raises an interrupt and measures, in fabric clock
// cycles, how long software takes to acknowledge it. Project RTL.
//
// Two identical channels so that two software contexts can be measured independently
// (channel 0 -> Linux, channel 1 -> FreeRTOS in this project). Everything runs in PCLK
// (FIC3 clock, 50 MHz on the Discovery Kit reference design: 1 cycle = 20 ns).
//
// The interrupt is level: it stays high from the event until software writes ACK.
// Latency = PCLK cycles from the first cycle IRQ is high to the APB write of ACK.
//
// Register map per channel (channel 0 at 0x80, channel 1 at 0xC0 of the pfdaq APB slot):
//   +0x00 CTRL       RW  bit0 ENABLE (periodic events; cleared by hardware when COUNT_LIMIT is reached)
//                    WO  bit1 ONESHOT (pulse: one event now), bit2 CLEAR (pulse: clear all statistics)
//                    RW  [31:16] JITTER_MASK: a pseudo-random value ANDed with this mask is added
//                        to every interval, so events do not stay phase-locked to a software tick
//   +0x04 PERIOD     RW  PCLK cycles between events (minimum 2)
//   +0x08 COUNT_LIMIT RW events to generate after ENABLE is set; 0 = no limit
//   +0x0C ACK        WO  any write acknowledges the pending interrupt
//   +0x10 STATUS     RO  bit0 interrupt pending, bit1 ENABLE, bit2 CLEAR in progress (64 cycles;
//                        no events are raised while it runs)
//   +0x14 EVENTS     RO  interrupts raised
//   +0x18 ACKS       RO  acknowledges recorded (latency samples)
//   +0x1C OVERRUN    RO  events that became due while the previous one was still pending (not raised)
//   +0x20 LAT_LAST   RO  last latency, cycles
//   +0x24 LAT_MIN    RO  minimum since CLEAR (0xFFFFFFFF if no sample)
//   +0x28 LAT_MAX    RO  maximum since CLEAR
//   +0x2C LAT_SUM_LO RO  sum of latencies, low word
//   +0x30 LAT_SUM_HI RO  sum of latencies, high word
//   +0x34 HIST_SEL   RW  histogram bin to read, 0..63
//   +0x38 HIST_DATA  RO  samples in bin HIST_SEL (valid 2 cycles after HIST_SEL is written)
//   +0x3C SPURIOUS   RO  ACK writes with no interrupt pending
//
// The histogram lives in two small RAMs written identically: one is read-modified-written
// for each recorded sample, the other is read for HIST_DATA, so the two never collide.
// Histogram bins (two per octave): latency 0 -> bin 0, 1 -> bin 1; otherwise with m = index of
// the most significant set bit, bin = 2*m + (bit m-1). Bin b >= 2 therefore covers
// [2^m + s*2^(m-1), 2^m + (s+1)*2^(m-1) - 1] with m = b/2, s = b%2.

`timescale 1ns / 1ps

module pfdaq_latency_ch (
    input             clk,
    input             rstn,
    input             wr,          // APB write strobe for this channel
    input      [5:0]  addr,        // byte offset inside the channel, word aligned
    input      [31:0] wdata,
    output reg [31:0] rdata,
    output            irq
);
    reg         enable;
    reg  [15:0] jitter_mask;
    reg  [31:0] period;
    reg  [31:0] count_limit;
    reg  [5:0]  hist_sel;

    reg         pending;
    reg  [31:0] timer;        // cycles until the next event
    reg  [31:0] lat_cnt;      // cycles since IRQ went high
    reg  [31:0] run_events;   // events since ENABLE was set (for COUNT_LIMIT)
    reg  [31:0] events, acks, overrun, spurious;
    reg  [31:0] lat_last, lat_min, lat_max;
    reg  [63:0] lat_sum;
    reg  [15:0] lfsr;

    // histogram RAMs (no reset: cleared by a 64-cycle sweep)
    reg  [31:0] hist_a [0:63];   // read-modify-write copy
    reg  [31:0] hist_b [0:63];   // APB read copy
    reg  [31:0] ha_q, hb_q;
    reg  [5:0]  inc_bin;
    reg         inc_pend;
    reg         clearing;
    reg  [5:0]  clr_idx;
    reg         hw_en;
    reg  [5:0]  hw_addr;
    reg  [31:0] hw_data;

    wire wr_ctrl = wr && (addr == 6'h00);
    wire wr_ack  = wr && (addr == 6'h0C);
    wire clear   = wr_ctrl && wdata[2];
    wire oneshot = wr_ctrl && wdata[1];

    assign irq = pending;

    // histogram bin of the latency being recorded
    function [5:0] bin_of;
        input [31:0] v;
        integer i;
        reg [4:0] m;
        begin
            m = 5'd0;
            for (i = 1; i < 32; i = i + 1)
                if (v[i]) m = i[4:0];
            if (v[31:1] == 31'd0)
                bin_of = {5'd0, v[0]};
            else
                bin_of = {m, v[m - 5'd1]};
        end
    endfunction

    // latency of the acknowledge being written now: IRQ has been high for lat_cnt + 1 cycles
    wire [31:0] lat_now  = lat_cnt + 32'd1;
    wire [5:0]  lat_bin  = bin_of(lat_now);
    // reload value: an event every PERIOD (+ jitter) cycles
    wire [31:0] interval = period + {16'd0, lfsr & jitter_mask} - 32'd1;
    wire        due      = enable && (timer == 32'd0) && !clearing;
    wire        limit_hit = (count_limit != 32'd0) && (run_events + 32'd1 >= count_limit);

    // RAM ports: one write port shared by increment and clear, two read ports
    always @(posedge clk) begin
        if (hw_en) begin
            hist_a[hw_addr] <= hw_data;
            hist_b[hw_addr] <= hw_data;
        end
        ha_q <= hist_a[lat_bin];
        hb_q <= hist_b[hist_sel];
    end

    always @(*) begin
        hw_en   = 1'b0;
        hw_addr = inc_bin;
        hw_data = ha_q + 32'd1;
        if (clearing) begin
            hw_en   = 1'b1;
            hw_addr = clr_idx;
            hw_data = 32'd0;
        end else if (inc_pend) begin
            hw_en   = 1'b1;
        end
    end

    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            enable <= 1'b0; jitter_mask <= 16'd0; period <= 32'd50000; count_limit <= 32'd0;
            hist_sel <= 6'd0; pending <= 1'b0; timer <= 32'd0; lat_cnt <= 32'd0; run_events <= 32'd0;
            events <= 32'd0; acks <= 32'd0; overrun <= 32'd0; spurious <= 32'd0;
            lat_last <= 32'd0; lat_min <= 32'hFFFFFFFF; lat_max <= 32'd0; lat_sum <= 64'd0;
            lfsr <= 16'hACE1;
            inc_bin <= 6'd0; inc_pend <= 1'b0; clearing <= 1'b1; clr_idx <= 6'd0;
        end else begin
            // histogram: the RAM read of lat_bin happens in the ACK cycle, the +1 write one cycle later
            inc_pend <= 1'b0;
            if (clearing) begin
                clr_idx <= clr_idx + 6'd1;
                if (clr_idx == 6'd63)
                    clearing <= 1'b0;
            end
            lfsr <= {lfsr[14:0], lfsr[15] ^ lfsr[13] ^ lfsr[12] ^ lfsr[10]};

            // configuration writes
            if (wr) begin
                case (addr)
                    6'h00: begin
                        enable      <= wdata[0];
                        jitter_mask <= wdata[31:16];
                        if (wdata[0] && !enable) begin
                            run_events <= 32'd0;
                            timer      <= interval;
                        end
                    end
                    6'h04: period      <= (wdata < 32'd2) ? 32'd2 : wdata;
                    6'h08: count_limit <= wdata;
                    6'h34: hist_sel    <= wdata[5:0];
                    default: ;
                endcase
            end

            // latency counter
            if (pending)
                lat_cnt <= lat_cnt + 32'd1;

            // acknowledge
            if (wr_ack) begin
                if (pending) begin
                    pending  <= 1'b0;
                    acks     <= acks + 32'd1;
                    lat_last <= lat_now;
                    if (lat_now < lat_min) lat_min <= lat_now;
                    if (lat_now > lat_max) lat_max <= lat_now;
                    lat_sum  <= lat_sum + {32'd0, lat_now};
                    inc_bin  <= lat_bin;
                    inc_pend <= 1'b1;
                end else begin
                    spurious <= spurious + 32'd1;
                end
            end

            // event generation
            if (enable && !(wr_ctrl && !wdata[0])) begin
                if (due) begin
                    timer <= interval;
                    if (pending && !wr_ack) begin
                        overrun <= overrun + 32'd1;
                    end else begin
                        pending    <= 1'b1;
                        lat_cnt    <= 32'd0;
                        events     <= events + 32'd1;
                        run_events <= run_events + 32'd1;
                        if (limit_hit)
                            enable <= 1'b0;
                    end
                end else if ((timer != 32'd0) && !(wr_ctrl && wdata[0] && !enable)) begin
                    timer <= timer - 32'd1;
                end
            end
            if (oneshot && !pending && !clearing) begin
                pending <= 1'b1;
                lat_cnt <= 32'd0;
                events  <= events + 32'd1;
            end

            if (clear) begin
                events <= 32'd0; acks <= 32'd0; overrun <= 32'd0; spurious <= 32'd0;
                lat_last <= 32'd0; lat_min <= 32'hFFFFFFFF; lat_max <= 32'd0; lat_sum <= 64'd0;
                clearing <= 1'b1;
                clr_idx  <= 6'd0;
                inc_pend <= 1'b0;
            end
        end
    end

    always @(*) begin
        case (addr)
            6'h00:   rdata = {jitter_mask, 15'd0, enable};
            6'h04:   rdata = period;
            6'h08:   rdata = count_limit;
            6'h10:   rdata = {29'd0, clearing, enable, pending};
            6'h14:   rdata = events;
            6'h18:   rdata = acks;
            6'h1C:   rdata = overrun;
            6'h20:   rdata = lat_last;
            6'h24:   rdata = lat_min;
            6'h28:   rdata = lat_max;
            6'h2C:   rdata = lat_sum[31:0];
            6'h30:   rdata = lat_sum[63:32];
            6'h34:   rdata = {26'd0, hist_sel};
            6'h38:   rdata = hb_q;
            6'h3C:   rdata = spurious;
            default: rdata = 32'd0;
        endcase
    end
endmodule
