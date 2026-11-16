// pfdaq_demo_regs - a small block to turn into a Libero core by hand.
//   An APB slave with five 32-bit registers, and an AXI4-Stream initiator
//   that sends LEN words (0, 1, 2, ...) when CTRL.start is written.
//   One clock and one reset for both buses, so there is no clock crossing.
//
//   0x00 CTRL    bit 0 write 1: start a burst (reads back 0)
//   0x04 LEN     number of words in a burst, 1 or more (reset value 4)
//   0x08 STATUS  bit 0 busy, bit 1 done (cleared by the next start)
//   0x0C SCRATCH read and write, no other effect
//   0x10 SENT    words sent in the current or last burst (read only)
`timescale 1ns / 1ps

module pfdaq_demo_regs #(
    parameter [31:0] DEFAULT_LEN = 32'd4
) (
    // APB slave, standard AMBA names
    input             PCLK,
    input             PRESETN,
    input             PSEL,
    input             PENABLE,
    input             PWRITE,
    input      [31:0] PADDR,
    input      [31:0] PWDATA,
    output reg [31:0] PRDATA,
    output            PREADY,
    output            PSLVERR,
    // AXI4-Stream initiator, standard AMBA names (same clock as the APB side)
    output     [31:0] TDATA,
    output            TVALID,
    input             TREADY,
    output            TLAST
);
    reg [31:0] len, scratch, sent;
    reg        busy, done;

    wire wr = PSEL & PENABLE & PWRITE;
    wire rd = PSEL & ~PWRITE;

    assign PREADY  = 1'b1;
    assign PSLVERR = 1'b0;

    assign TVALID = busy;
    assign TDATA  = sent;
    assign TLAST  = busy & (sent == len - 32'd1);

    always @(posedge PCLK or negedge PRESETN) begin
        if (!PRESETN) begin
            len <= DEFAULT_LEN; scratch <= 32'd0; sent <= 32'd0; busy <= 1'b0; done <= 1'b0;
        end else begin
            if (wr && PADDR[4:2] == 3'd1) len     <= PWDATA;
            if (wr && PADDR[4:2] == 3'd3) scratch <= PWDATA;
            if (wr && PADDR[4:2] == 3'd0 && PWDATA[0] && !busy && len != 32'd0) begin
                busy <= 1'b1; done <= 1'b0; sent <= 32'd0;
            end else if (busy && TREADY) begin
                sent <= sent + 32'd1;
                if (TLAST) begin busy <= 1'b0; done <= 1'b1; end
            end
        end
    end

    always @* begin
        case (PADDR[4:2])
            3'd0:    PRDATA = 32'd0;
            3'd1:    PRDATA = len;
            3'd2:    PRDATA = {30'd0, done, busy};
            3'd3:    PRDATA = scratch;
            3'd4:    PRDATA = sent;
            default: PRDATA = 32'd0;
        endcase
    end
endmodule
