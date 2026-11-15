# pfdaq fabric: simulation, build and programming entry points.
#
#   make sim                     both testbenches, each checked by its independent Python model
#   make fabric DESIGN_VERSION=n Libero build (needs Libero SoC 2026.1 and a licence)
#   make program JOB=file.job    FlashPro Express: FABRIC + SNVM only
#
# Simulation needs only Icarus Verilog and Python 3.
OUT ?= ../build/sim
IVERILOG ?= iverilog
VVP ?= vvp
PYTHON ?= python3

.PHONY: sim sim-stream sim-latency fabric program clean
sim: sim-stream sim-latency

sim-stream:
	@mkdir -p $(OUT)
	$(IVERILOG) -g2005 -o $(OUT)/tb_stream.vvp rtl/pfdaq_stream_src.v tb/tb_pfdaq_stream_src.v
	cd $(OUT) && mkdir -p sim && $(VVP) -n tb_stream.vvp > stream.log
	$(PYTHON) tb/check_trace.py $(OUT)/sim/trace.txt | tee $(OUT)/result_stream.txt
	@tail -1 $(OUT)/result_stream.txt | grep -q PASS

sim-latency:
	@mkdir -p $(OUT)
	$(IVERILOG) -g2005 -o $(OUT)/tb_latency.vvp rtl/pfdaq_core.v rtl/pfdaq_stream_src.v rtl/pfdaq_latency.v tb/tb_pfdaq_latency.v
	cd $(OUT) && mkdir -p sim && $(VVP) -n tb_latency.vvp > latency.log
	$(PYTHON) tb/check_latency.py $(OUT)/sim/lat_trace.txt | tee $(OUT)/result_latency.txt
	@tail -1 $(OUT)/result_latency.txt | grep -q PASS

fabric:
	libero/build.sh $(DESIGN_VERSION)

program:
	libero/program.sh $(JOB)

clean:
	rm -rf $(OUT)
