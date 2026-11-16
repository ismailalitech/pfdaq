# amp-station: a four-core monitoring station

All four cores of the board do different jobs and talk to each other: Linux cpu 0 acquires frames from the fabric, cpu 1 analyses them and owns the RPMsg link, cpu 2 shows a dashboard and takes operator commands, and the FreeRTOS supervisor on hart 4 holds the limits, watches the heartbeat and stops the source when something is wrong. The page that explains it step by step is [M10](../../docs/modules/M10-four-core-monitoring-station.md).

**Status: experimental.** Everything below was built and tested on the PC (host tests, a RISC-V user-mode emulator, offline checks of the payload). The run on the board has not been made yet; nothing here claims a board result.

## What is in this folder

| File | What it is |
|---|---|
| `station.c`, `station_common.h`, `station_link.h`, `station_fabric.h` | the Linux program: roles `init`, `acq`, `ana`, `tel`, `hello`, `listen`, `summary`; the ring, the frame model and features, the RPMsg link, the fabric source (hardware and simulated) |
| `../../firmware/u54_4-supervisor/pfdaq_station.h` | the part that runs on hart 4: messages, limits, trip, reset, roll call. Plain C with no hardware access, so the PC tests run this same file |
| `../../firmware/u54_4-supervisor/pfdaq_supervisor.c` | three small blocks under `#ifdef PFDAQ_AMP_DEMO`; without the switch the file builds to the proven image |
| `build_firmware.sh` | builds the demo ELF and `payload.bin` into `build/amp-demo/` and checks them offline |
| `verify_payload.py` | the offline comparison of a payload with the proven one |
| `deploy_payload.sh` | backup, install and restore of the payload partition (for board day, not run yet) |
| `run_demo.sh` | the board-day script: runs the whole scenario and records three panes |
| `board/start_station.sh`, `board/stop_station.sh` | start and stop the three processes on the board |
| `capture/` | timestamping of streams, UART capture, and the merge into the replay recording |
| `fake_hart4.c` | stand-in for hart 4 on the PC: the real `pfdaq_station.h` behind a socket |
| `tests/` | unit tests, end-to-end tests, tests of the scripts, and the generator of the figures |

## Build

```sh
make -C experiments/amp-station host        # x86, for the tests
make -C experiments/amp-station board       # static RISC-V Linux binary for the board
experiments/amp-station/build_firmware.sh   # hart 4 firmware and payload, into build/amp-demo/
```

`build_firmware.sh` uses the compiler the proven firmware was built with (the Yocto cross GCC 13.4, taken read-only from `yocto-dev`), builds the production supervisor first and stops unless its loadable image is byte-identical to `images/pfdaq-v051/polarfire-soc-freertos-amp-demo.elf`, then builds the demo with `-DPFDAQ_AMP_DEMO`. It does not run bitbake and does not write to the Yocto deploy directory.

## Test (no board)

```sh
experiments/amp-station/tests/run_tests.sh        # unit, model, end to end with the simulated source, start and stop scripts (about 80 s)
experiments/amp-station/tests/test_run_demo.sh    # the board-day script against stand-ins (about 40 s)
experiments/amp-station/tests/test_deploy.sh      # deploy_payload.sh against a plain file
python3 experiments/amp-station/tests/test_capture.py
```

## Board day

1. `deploy_payload.sh backup`, then `deploy_payload.sh install`, then reboot the board and check the hart 4 console.
2. `make -C experiments/amp-station board`, then `run_demo.sh` (it copies the binary and the two scripts to `/root` itself).
3. To go back: `deploy_payload.sh restore`, reboot.

Read the header of each script first: both stop and tell you what they are about to do.

## Using the recording as a figure

`run_demo.sh` writes `logs/m5/station/<time>/recording.jsonl`. Copy it to `docs/diagrams/recordings/m10-station.jsonl` and add this entry to `docs/diagrams/labs-m10-replay.yaml` (a new file, so `make_figures.py` does not overwrite it):

```yaml
- id: lab-m10-station-run
  type: replay
  alt: "..."
  width: 900
  recording: recordings/m10-station.jsonl
  panes:
    - {title: "Linux cpu 2: operator session and dashboard (ssh)", rows: 9}
    - {title: "Linux cpu 0 and cpu 1: acquisition and analysis logs (ssh)", rows: 6}
    - {title: "hart 4 console (UART)", rows: 6}
  callouts:
    - {t: 0.0, title: "...", text: "...", panes: [0]}
```

Choose the callout times from the recording itself (they are recorded seconds).
