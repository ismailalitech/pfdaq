`timescale 1ns/1ps
module tb_lost;
  reg clk=0, rstn=0, wr=0; reg [5:0] addr=0; reg [31:0] wdata=0; wire [31:0] rdata; wire irq;
  integer cyc=0, i, tot, G1, G2;
  always #10 clk=~clk;
  always @(posedge clk) cyc<=cyc+1;
  pfdaq_latency_ch dut(.clk(clk),.rstn(rstn),.wr(wr),.addr(addr),.wdata(wdata),.rdata(rdata),.irq(irq));
  task w(input [5:0] a, input [31:0] d); begin
    @(posedge clk); wr<=1; addr<=a; wdata<=d; @(posedge clk); wr<=0; end endtask
  task total; begin tot=0; for(i=0;i<64;i=i+1) tot=tot+dut.hist_a[i]; end endtask
  initial begin
    if(!$value$plusargs("G1=%d",G1)) G1=10; if(!$value$plusargs("G2=%d",G2)) G2=3;
    $dumpfile("lost.vcd"); $dumpvars(0,tb_lost.dut); $dumpvars(0,tb_lost.cyc);
    repeat(3) @(posedge clk); rstn=1; repeat(80) @(posedge clk);
    w(6'h00, 32'h2);                       // ONESHOT
    repeat(G1-1) @(posedge clk);
    w(6'h00, 32'h4);                       // CLEAR
    repeat(G2-1) @(posedge clk);
    w(6'h0C, 32'h1);                       // ACK during the sweep
    repeat(100) @(posedge clk);
    total; $display("ACKS %0d LAT_LAST %0d histogram total %0d events %0d", dut.acks, dut.lat_last, tot, dut.events);
    $finish;
  end
endmodule
