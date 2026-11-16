// pfdaq_demo_master - a tiny APB master that stands in for the processor in the scratch design.
//   After reset it writes LEN = 8, then CTRL = 1 (start a burst of pfdaq_demo_regs), then stops.
//   It exists so that the design has a driver on the register bus and fits the device's pin count;
//   on the board the same bus is driven by the processor through a fabric interface.
`timescale 1ns / 1ps

module pfdaq_demo_master (
    input             PCLK,
    input             PRESETN,
    output reg        PSEL,
    output reg        PENABLE,
    output            PWRITE,
    output reg [31:0] PADDR,
    output reg [31:0] PWDATA,
    input      [31:0] PRDATA,
    input             PREADY,
    input             PSLVERR
);
    // 0 wait, 1 setup LEN, 2 access LEN, 3 setup CTRL, 4 access CTRL, 5 done
    reg [2:0]  state;
    reg [3:0]  delay;
    assign PWRITE = 1'b1;

    always @(posedge PCLK or negedge PRESETN) begin
        if (!PRESETN) begin
            state <= 3'd0; delay <= 4'd0; PSEL <= 1'b0; PENABLE <= 1'b0; PADDR <= 32'd0; PWDATA <= 32'd0;
        end else case (state)
            3'd0: begin delay <= delay + 4'd1; if (delay == 4'd15) state <= 3'd1; end
            3'd1: begin PSEL <= 1'b1; PENABLE <= 1'b0; PADDR <= 32'h04; PWDATA <= 32'd8; state <= 3'd2; end
            3'd2: begin PENABLE <= 1'b1; if (PENABLE && PREADY) begin PSEL <= 1'b0; PENABLE <= 1'b0; state <= 3'd3; end end
            3'd3: begin PSEL <= 1'b1; PENABLE <= 1'b0; PADDR <= 32'h00; PWDATA <= 32'd1; state <= 3'd4; end
            3'd4: begin PENABLE <= 1'b1; if (PENABLE && PREADY) begin PSEL <= 1'b0; PENABLE <= 1'b0; state <= 3'd5; end end
            default: ;
        endcase
    end
endmodule
