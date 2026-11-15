module latency_dump;
  initial begin
    $dumpfile("lat.vcd");
    $dumpvars(0, tb_pfdaq_latency.dut.u_lat0);
    $dumpvars(0, tb_pfdaq_latency.dut.u_lat1);
    $dumpvars(0, tb_pfdaq_latency.cyc);
  end
endmodule
