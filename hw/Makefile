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

# Protocol checks and functional coverage on the stream source, watching the unchanged stream testbench.
# ASSERT_RTL and ASSERT_TB can point at scratch copies of the RTL (to plant a bug and see the checks fire)
# and of the stream testbench (to add runs and see the coverage change).
# Needs Icarus Verilog 12 (final blocks, -g2012). The existing check_trace.py also judges the same trace.
ASSERT_RTL ?= rtl/pfdaq_stream_src.v
ASSERT_TB ?= tb/tb_pfdaq_stream_src.v
.PHONY: sim-assert
sim-assert:
	@mkdir -p $(OUT)
	$(IVERILOG) -g2012 -s tb_pfdaq_assert -o $(OUT)/tb_assert.vvp $(ASSERT_RTL) $(ASSERT_TB) tb/pfdaq_stream_checks.v tb/pfdaq_stream_cover.v tb/tb_pfdaq_assert.v
	cd $(OUT) && mkdir -p sim && $(VVP) -n tb_assert.vvp > assert_log.txt
	grep -E '^(CHECK|ASSERT)' $(OUT)/assert_log.txt | tee $(OUT)/assert_results.txt
	grep '^COV' $(OUT)/assert_log.txt > $(OUT)/coverage.txt
	@tail -7 $(OUT)/coverage.txt
	@$(PYTHON) tb/check_trace.py $(OUT)/sim/trace.txt | grep -m1 '^RESULT' | sed 's/^/trace checker (check_trace.py): /' | tee $(OUT)/assert_trace_verdict.txt
	@tail -1 $(OUT)/assert_results.txt | grep -q PASS

fabric:
	libero/build.sh $(DESIGN_VERSION)

program:
	libero/program.sh $(JOB)

clean:
	rm -rf $(OUT)
