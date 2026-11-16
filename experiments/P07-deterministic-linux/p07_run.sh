#!/bin/sh
# P07: the latency measurement of P02 under runtime changes of interrupt affinity, CPU placement and priority.
# Runs on the board as root. Only runtime settings are changed (/proc/irq, taskset, chrt); all of them are
# restored on exit. Output: "label json" lines for the runs, "# ..." lines for the setting changes.
#
#   sh p07_run.sh user [runs]       user-space path (pfdaq_lat -c 0, 500 us period, 100,000 events per run)
#   sh p07_run.sh kernel [runs]     in-kernel acknowledge path (pfdaq_lat -k, 200 us period); binds pfdaq_uio
#   sh p07_run.sh noisy [runs]      a subset of the user-space conditions, to be run while ANOTHER machine logs in to the
#                                   board every 10 s (see README); shows what login traffic does to each setting
#   sh p07_run.sh quick             one short run of each kind of setting, to check the harness (about 1 minute)
MODE=${1:-user}
RUNS=${2:-5}
EVENTS=${EVENTS:-100000}
STATE=/tmp/p07_state
mkdir -p $STATE
LOADPIDS=$STATE/loadpids
: > $LOADPIDS

# ---- saved state and restoring it ---------------------------------------------------------------
save_state() {
	: > $STATE/irq_orig
	for f in /proc/irq/[0-9]*/smp_affinity; do echo "$f $(cat $f)" >> $STATE/irq_orig; done
	cat /proc/irq/default_smp_affinity > $STATE/irq_default
	: > $STATE/task_orig
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		m=$(taskset -p $pid 2>/dev/null | sed 's/.*: //')
		[ -n "$m" ] && echo "$pid $m" >> $STATE/task_orig
	done
	: > $STATE/sched_orig
	for t in $(ps -eo pid,comm | awk '/irq\/[0-9]+-pfdaq_stream_src/ {print $1}'); do
		echo "$t $(chrt -p $t | sed -n 's/.*policy: //p' | tr '\n' ' ') $(chrt -p $t | sed -n 's/.*priority: //p' | tr '\n' ' ')" >> $STATE/sched_orig
	done
}
restore_state() {
	load_stop
	while read f m; do echo $m > $f 2>/dev/null; done < $STATE/irq_orig
	cat $STATE/irq_default > /proc/irq/default_smp_affinity 2>/dev/null
	while read pid m; do taskset -a -p $m $pid >/dev/null 2>&1; done < $STATE/task_orig
	while read t pol pr; do
		case "$pol" in SCHED_FIFO*) chrt -f -p $pr $t 2>/dev/null ;; SCHED_RR*) chrt -r -p $pr $t 2>/dev/null ;; *) chrt -o -p 0 $t 2>/dev/null ;; esac
	done < $STATE/sched_orig
	echo "# state restored"
}
trap 'restore_state; exit 1' INT TERM
trap 'restore_state' EXIT

# ---- helpers --------------------------------------------------------------------------------------
irq_of() { awk -v n="$1" '$NF == n { sub(":", "", $1); print $1; exit }' /proc/interrupts; }
irq_counts() { awk -v n="$1" '$NF == n { printf "%s/%s/%s", $2, $3, $4 }' /proc/interrupts; }
irq_thread() { ps -eo pid,comm | awk -v n="-$1\$" '$2 ~ /^irq\// && $2 ~ n { print $1; exit }'; }
say() { while read l; do echo "$1 $l"; done; }

steer_irq() {          # steer_irq <hex mask> : the fabric interrupt only
	IRQ=$(irq_of pfdaq_stream_src)
	echo "$1" > /proc/irq/$IRQ/smp_affinity
	echo "# irq $IRQ affinity $1, effective $(cat /proc/irq/$IRQ/effective_affinity)"
}
steer_others() {       # steer_others <hex mask> : every other interrupt that allows it
	for d in /proc/irq/[0-9]*; do
		i=${d#/proc/irq/}
		[ "$i" = "$IRQ" ] && continue
		echo "$1" > $d/smp_affinity 2>/dev/null
	done
	echo "$1" > /proc/irq/default_smp_affinity 2>/dev/null
	echo "# all other movable interrupts and the default affinity: $1"
}
move_tasks() {         # move_tasks <hex mask> : every task (threads included) that allows it
	n=0
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		taskset -a -p $1 $pid >/dev/null 2>&1 && n=$((n+1))
	done
	echo "# tasks moved to mask $1: $n of $(ls -d /proc/[0-9]* | wc -l) processes accepted"
}
load_start() {         # load_start all | cpu0 | cpu1 | cpu2 | pinned01
	case "$1" in
	all)      for i in 1 2 3; do (while :; do :; done) & echo $! >> $LOADPIDS; done
	          (while :; do pfdaq_cap -w 252 -f 1024 -n 64 >/dev/null 2>&1; done) & echo $! >> $LOADPIDS ;;
	cpu2)     taskset -c 2 sh -c 'while :; do :; done' & echo $! >> $LOADPIDS ;;
	pinned01) taskset -c 0 sh -c 'while :; do :; done' & echo $! >> $LOADPIDS
	          taskset -c 1 sh -c 'while :; do :; done' & echo $! >> $LOADPIDS
	          taskset -c 0,1 sh -c 'while :; do pfdaq_cap -w 252 -f 1024 -n 64 >/dev/null 2>&1; done' & echo $! >> $LOADPIDS ;;
	esac
	echo "# load started: $1"
	sleep 1
}
load_stop() {
	for p in $(cat $LOADPIDS 2>/dev/null); do kill $p 2>/dev/null; done
	: > $LOADPIDS
	killall pfdaq_cap 2>/dev/null
	true
}

# user-space runs: cond <label> <taskset cpu list or -> <extra pfdaq_lat args>
cond() {
	label=$1; cpus=$2; shift 2
	i=1
	while [ $i -le $RUNS ]; do
		b=$(irq_counts pfdaq_stream_src)
		if [ "$cpus" = "-" ]; then pfdaq_lat -c 0 -p 500 -n $EVENTS "$@" > $STATE/out 2>&1
		else taskset -c $cpus pfdaq_lat -c 0 -p 500 -n $EVENTS "$@" > $STATE/out 2>&1; fi
		a=$(irq_counts pfdaq_stream_src)
		say "$label" < $STATE/out
		echo "# irq-counts $label run $i before(cpu0/1/2) $b after $a"
		i=$((i+1))
	done
}
kcond() {              # in-kernel acknowledge: kcond <label>
	label=$1; i=1
	while [ $i -le $RUNS ]; do
		b=$(irq_counts $KNAME)
		pfdaq_lat -k -p 200 -n $EVENTS > $STATE/out 2>&1
		a=$(irq_counts $KNAME)
		say "$label" < $STATE/out
		echo "# irq-counts $label run $i before(cpu0/1/2) $b after $a"
		i=$((i+1))
	done
}

IRQ=$(irq_of pfdaq_stream_src)
save_state
echo "# p07 $MODE runs=$RUNS events=$EVENTS $(date -u +%FT%TZ)"
echo "# kernel $(uname -r), irq default affinity $(cat $STATE/irq_default)"

if [ "$MODE" = quick ]; then
	EVENTS=2000; RUNS=1
	cond u1_default - 
	cond u2_fifo - -R 80
	steer_irq 2; cond u5_irq1_cpu1 1 -R 80; steer_irq 1
	exit 0
fi

if [ "$MODE" = user ]; then
	cond u1_default_idle -
	cond u2_fifo80_idle - -R 80
	cond u3_fifo80_cpu0 0 -R 80
	cond u4_fifo80_cpu1_irq_cpu0 1 -R 80
	steer_irq 2; cond u5_fifo80_cpu1_irq_cpu1 1 -R 80
	steer_irq 4; cond u6_fifo80_cpu2_irq_cpu2 2 -R 80
	T=$(irq_thread pfdaq_stream_src)
	if [ -n "$T" ]; then
		chrt -f -p 90 $T; echo "# irq thread $T now: $(chrt -p $T | tr '\n' ' ')"
		cond u7_fifo80_cpu2_irq_cpu2_irqthread90 2 -R 80
		chrt -f -p 50 $T
	fi
	steer_irq 7
	load_start all;   cond u8_loaded_default -
	cond u9_loaded_fifo80 - -R 80
	load_stop
	load_start cpu2;  steer_irq 4
	cond u10_cpu2busy_default 2
	cond u11_cpu2busy_fifo80 2 -R 80
	load_stop; steer_irq 7
	# isolated: everything else on CPUs 0 and 1, the fabric interrupt and the measuring program alone on CPU 2
	steer_others 3; steer_irq 4; move_tasks 3
	load_start pinned01
	cond u12_isolated_loaded_fifo80 2 -R 80
	load_stop
fi

if [ "$MODE" = noisy ]; then
	cond n1_default -
	cond n2_fifo80 - -R 80
	steer_irq 2; cond n5_fifo80_cpu1_irq_cpu1 1 -R 80
	steer_irq 4; cond n6_fifo80_cpu2_irq_cpu2 2 -R 80
	steer_irq 7
	steer_others 3; steer_irq 4; move_tasks 3
	cond n12_isolated_fifo80 2 -R 80
fi

if [ "$MODE" = kernel ]; then
	D=40000f00.uio
	modprobe pfdaq_uio
	echo $D > /sys/bus/platform/drivers/uio_pdrv_genirq/unbind
	echo pfdaq_uio > /sys/bus/platform/devices/$D/driver_override
	echo $D > /sys/bus/platform/drivers/pfdaq_uio/bind
	sleep 1
	KNAME=$(awk '/PLIC +119 /{print $NF}' /proc/interrupts)
	IRQ=$(irq_of $KNAME)
	echo "# kernel-ack mode: irq $IRQ name $KNAME, thread $(irq_thread $KNAME)"
	kcond k1_default_idle
	steer_irq 2; kcond k2_irq_cpu1
	steer_irq 4; kcond k3_irq_cpu2
	steer_irq 7
	load_start all; kcond k4_loaded_default; load_stop
	steer_others 3; steer_irq 4; move_tasks 3
	load_start pinned01; kcond k5_isolated_loaded; load_stop
	restore_state
	echo $D > /sys/bus/platform/drivers/pfdaq_uio/unbind
	echo uio_pdrv_genirq > /sys/bus/platform/devices/$D/driver_override
	echo $D > /sys/bus/platform/drivers/uio_pdrv_genirq/bind
	sleep 1
	echo > /sys/bus/platform/devices/$D/driver_override
fi
