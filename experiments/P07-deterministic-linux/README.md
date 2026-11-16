# P07: interrupt affinity, CPU placement and priorities against the Linux latency stalls

Belongs to module [P07, making Linux more deterministic](../../docs/modules/P07-deterministic-linux.md).

`p07_run.sh` repeats the interrupt-latency measurement of module P02 (`pfdaq_lat`, channel 0, answered by Linux) under changes that can be made while the system runs, and puts every setting back when it ends:

- the **interrupt affinity** of the fabric interrupt (`/proc/irq/N/smp_affinity`), and of all other interrupts that allow it;
- the **CPU placement** of the measuring program and of every other task (`taskset`);
- **priorities**: `SCHED_FIFO` and locked memory for the program (`pfdaq_lat -R 80`), and the priority of the kernel thread that serves the interrupt (`chrt`);
- three kinds of load: P02's load (one busy loop per CPU plus a capture that never stops), one busy loop on the program's own CPU, and the same load kept on the other CPUs.

Nothing here changes the boot arguments, the U-Boot environment or any file on the SD card; the kernel has no `CONFIG_CPUSETS` and no `nohz_full`, so `taskset` and `/proc/irq` are the available tools.

```sh
scp p07_run.sh root@<board>:/tmp/
ssh root@<board> 'cd /tmp && sh p07_run.sh quick'            # one short run per kind of setting, about a minute
ssh root@<board> 'cd /tmp && nohup sh p07_run.sh user 5 > /tmp/p07_user.txt 2>&1 &'      # about 55 minutes
ssh root@<board> 'cd /tmp && nohup sh p07_run.sh kernel 5 > /tmp/p07_kernel.txt 2>&1 &'  # about 10 minutes
```

**Do not log in to the board, or load it in any other way, while a measurement runs.** An ssh login is network traffic and a burst of work for the processor, and the measurement sees it. Start the run, wait for its expected duration, and read the result file afterwards.

Output: `label json` lines for the runs and `#` lines for each setting that was changed and for the per-CPU interrupt counts before and after each run.
