`timescale 1ns / 1ps
module stream_dump;
 initial begin
 $dumpfile("dump.vcd");
 $dumpvars(0, tb_pfdaq_stream_src);
 $dumpoff;
 #1500 $dumpon; #2100 $dumpoff;
 #800 $dumpon; #1200 $dumpoff;
 #105400 $dumpon; #1600 $dumpoff;
 #27400 $dumpon; #7500 $dumpoff;
 #24500 $dumpon; #4500 $dumpoff;
 end
endmodule
