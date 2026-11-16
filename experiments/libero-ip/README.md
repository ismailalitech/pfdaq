# libero-ip: Verilog turned into Libero cores

Belongs to module [F07, Create your own IP in Libero](../../docs/modules/F07-create-your-own-ip-in-libero.md), which walks through the Libero GUI steps with screenshots. Nothing here is part of the board fabric; it is a small scratch design you can copy.

| File | What it is |
|---|---|
| `pfdaq_demo_regs.v` | An APB slave with five 32-bit registers and an AXI4-Stream initiator that sends a burst of words on request. The block you turn into a core. |
| `pfdaq_demo_master.v` | A tiny APB master that writes the length and the start bit once after reset. It stands in for the processor. |
| `tb_pfdaq_demo_regs.v` | Self-checking test of the register block alone, with a receiver that stalls. |
| `tb_pfdaq_demo_top.v` | Self-checking test of master and register block together. |
| `create_ip_demo.tcl` | The whole by-hand flow as one Libero script: empty project, import, two cores with bus interfaces, SmartDesign, generate, synthesize. |
| `make_cpz.py` | Writes `pfdaq_demo_regs-1.0.0.cpz`, a package of the register core for Libero's Add Core to Vault; with `--repack` it packs a vendor core that is already in a vault. |
| `create_ip_catalog_demo.tcl` | The Catalog route as one script: scratch vault, the two packages, a new project, the register core placed from the vault, generate, synthesize. |

## What it needs

- Icarus Verilog for the two tests (`iverilog -g2005`).
- Libero SoC 2026.1 and its licence for the scripts. CoreAPB3 comes from the IP vault, so the vault must hold it.
- Python 3 for `make_cpz.py`.

## Run

```text
cd experiments/libero-ip
iverilog -g2005 -o /tmp/tb1.vvp tb_pfdaq_demo_regs.v pfdaq_demo_regs.v && vvp /tmp/tb1.vvp
iverilog -g2005 -o /tmp/tb2.vvp tb_pfdaq_demo_top.v pfdaq_demo_master.v pfdaq_demo_regs.v && vvp /tmp/tb2.vvp
mkdir -p /tmp/ipdemo && cd /tmp/ipdemo
libero SCRIPT:<path to this folder>/create_ip_demo.tcl
```

The Catalog route (see the header of `create_ip_catalog_demo.tcl`) changes the vault location of every Microchip tool of your user; use a scratch folder and set the location back afterwards.

The tests end with `PASS: ...`. The script ends with `Synthesis completed.` and `The Execute Script command succeeded.`; the project is left in `/tmp/ipdemo/libero-ip-demo`.
