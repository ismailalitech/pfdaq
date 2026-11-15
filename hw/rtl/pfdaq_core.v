// pfdaq_core - top level of the project fabric block: stream source + latency unit behind
// one APB slot (FIC3, 0x40000F00 on the Discovery Kit build).
//   0x00..0x7F  pfdaq_stream_src registers
//   0x80..0xBF  latency channel 0 (interrupt IRQ_LAT0)
//   0xC0..0xFF  latency channel 1 (interrupt IRQ_LAT1)
`timescale 1ns / 1ps

module pfdaq_core (
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
    output            PSLVERR,
    output            IRQ_LAT0,
    output            IRQ_LAT1
);
    wire        sel_src  = PSEL & ~PADDR[7];
    wire        sel_lat0 = PSEL &  PADDR[7] & ~PADDR[6];
    wire        sel_lat1 = PSEL &  PADDR[7] &  PADDR[6];
    wire [31:0] prdata_src, prdata_lat0, prdata_lat1;
    wire [5:0]  ch_addr = {PADDR[5:2], 2'b00};

    pfdaq_stream_src #(.VERSION_VALUE(32'h00050000)) u_src (
        .ACLK(ACLK), .RSTN(RSTN), .TREADY(TREADY), .TDEST(TDEST), .TID(TID), .TKEEP(TKEEP),
        .TLAST(TLAST), .TVALID(TVALID), .TSTRB(TSTRB), .TDATA(TDATA),
        .PCLK(PCLK), .PRESETN(PRESETN), .PENABLE(PENABLE), .PSEL(sel_src),
        .PADDR({24'd0, PADDR[7:0]}), .PWRITE(PWRITE), .PWDATA(PWDATA),
        .PRDATA(prdata_src), .PREADY(), .PSLVERR());

    pfdaq_latency_ch u_lat0 (.clk(PCLK), .rstn(PRESETN), .wr(sel_lat0 & PENABLE & PWRITE),
        .addr(ch_addr), .wdata(PWDATA), .rdata(prdata_lat0), .irq(IRQ_LAT0));
    pfdaq_latency_ch u_lat1 (.clk(PCLK), .rstn(PRESETN), .wr(sel_lat1 & PENABLE & PWRITE),
        .addr(ch_addr), .wdata(PWDATA), .rdata(prdata_lat1), .irq(IRQ_LAT1));

    assign PRDATA  = PADDR[7] ? (PADDR[6] ? prdata_lat1 : prdata_lat0) : prdata_src;
    assign PREADY  = 1'b1;
    assign PSLVERR = 1'b0;
endmodule
