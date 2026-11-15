# Versions and compatibility

The baseline `pfdaq-v0.5.1` was built and validated with exactly these versions. Nothing else has been tried; a different version of any row is untested, not known to fail.

## Hardware

| Item | Value |
|---|---|
| Board | Microchip PolarFire SoC Discovery Kit (MPFS-DISCO-KIT) |
| Device | MPFS095T, FCSG325 package |
| Board revision | not recorded |
| Units tested | one |
| Boot flash (eNVM) content | Hart Software Services 0.99.53-v2026.04.1, as found on the board |

## Host tools

| Tool | Version | Used for |
|---|---|---|
| Ubuntu | 24.04, x86-64 | everything |
| Libero SoC | 2026.1 | fabric build; FlashPro Express for programming |
| Icarus Verilog | 12.0 | RTL simulation |
| Python | 3.12 | simulation models, test scripts, documentation tools (PyYAML) |
| SoftConsole RISC-V GCC | v2022.2 (GCC 8.3.0) | bare-metal firmware build outside Yocto |

## Fabric

| Item | Version |
|---|---|
| Discovery Kit reference design | v2026.04 |
| CoreAXI4DMAController | 2.2.107 |
| Mi-V IHC | 2.0.100 |
| pfdaq fabric | v0.5.1, design version 6, checksum D78D |

## Software on the board

| Item | Version |
|---|---|
| OpenSBI (inside HSS) | 1.2 |
| U-Boot | v2025.07-mchp (linux4microchip-2026.04) |
| Linux | 6.18.17 (linux4microchip-2026.04.1) |
| Yocto | Scarthgap: OpenEmbedded-Core `yocto-5.0.15`, meta-openembedded `ec0469748b` |
| meta-mchp | `linux4microchip-2026.04` |
| polarfire-soc-amp-examples | v2025.07 (FreeRTOS kernel V9.0.0, RPMsg-Lite) plus `firmware/u54_4-supervisor` |

## Fabric versions and what was validated on each

| Fabric | Design version | What changed | Board validation |
|---|---|---|---|
| v0.3.0 | 2 | two words per clock, burst capture | 13-test Python suite, ten-minute soak, fabric throughput |
| v0.4.0 | 3 | latency unit | latency matrix |
| v0.5.1 | 6 | payload modes, real-time mode, abort with zero fill, histograms in RAM | 27/27 suite (also from a fresh SD card), 8/8 bare-metal suite, 30 reboots |

Results that exist only for an older fabric are marked as such wherever they are quoted.

## Time limits

The Libero licence used for this build is node-locked and expires on 14 November 2027. Rebuilding the fabric after that date needs a new licence; the simulation, firmware and Linux builds do not depend on it.
