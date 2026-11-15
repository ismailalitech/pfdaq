# Writing guide

How pages in this repository are written, so that sixty of them read like one manual. The consistency check (`tools/check_content.py`) enforces the parts that can be checked by a program.

## Three kinds of page

| Kind | Directory | Purpose | Needs a board |
|---|---|---|---|
| Concept | `docs/concepts/` | The general idea, independent of this project: what it is, why it exists, the trade-offs | no |
| Module | `docs/modules/` | One topic as it is built here: how it works on this board, how to run it, what you should see | depends on the module |
| Case study | `docs/case-studies/` | One failure: symptom, how it was isolated, cause, fix, how to recognise it again | no |

A page stands on its own. A reader who arrives from a search result should be able to follow it after reading only the pages listed under "Read first".

## Front matter

Every page starts with YAML front matter. This is the only place a page's metadata lives; the catalog, the learning path, the system pages and the navigation block at the end of each page are generated from it.

```yaml
---
id: H02                       # track letter + number; never reused
title: "AXI4-Stream: the handshake, backpressure and TLAST"
track: fpga                   # fundamentals | boot | multicore | fpga | integration | performance | case-study | system | concept
level: 2                      # 1 fundamentals ... 5 advanced
summary: One sentence, shown in the catalog.
needs: [H01]                  # modules to read first; omit if none
repro: sim                    # sim | base | dedicated-fabric | full-system | doc | none
status: validated             # validated | validated-old | simulated | experimental | planned
hardware: [pfdaq-stream-src]  # node ids from docs/system/architecture.yaml
software: [pfdaq-cap]
sources:                      # path, or path:symbol,symbol - the check confirms each symbol exists
  - hw/rtl/pfdaq_stream_src.v:pfdaq_stream_src
evidence: [sim-stream]        # ids from evidence/<version>/manifest.yaml
related: [K03, I04]
---
```

`repro` says what a reader needs in order to run the page:

- `sim`: Icarus Verilog and Python only.
- `base`: the board with the shared baseline (fabric v0.5.1 and the pfdaq image). One build serves every such module.
- `dedicated-fabric`: its own fabric build.
- `full-system`: changes the boot flow or the integrated application.
- `doc`: explanation backed by recorded evidence; nothing to run.
- `none`: no tools at all (concept pages).

`status` says how far the content has been proven. `validated` means the steps on the page were executed and the results are in `evidence/`; it requires at least one evidence entry.

## Headings

Module pages use these headings, in this order. Leave one out only if it would be empty.

1. *(title, then one paragraph: what the reader will be able to do afterwards)*
2. **Before you start**: level, what to read first, tools.
3. **The idea**: the concept in general terms, short. Link to a concept page if there is one.
4. **On this board**: the blocks, addresses, signals and files involved, and the design choices made here.
5. **Run it**: numbered steps. Every command is one that was run.
6. **What you should see**: real output, trimmed. Say which lines matter.
7. **Evidence**: what was measured, under which conditions, and where the record is.
8. **When it goes wrong**: symptoms and causes that were really met.
9. **Trade-offs and going further**: what was given up, alternatives, exercises.

Case studies use: **Symptom**, **Isolation**, **Cause**, **Fix**, **How to recognise it**.
Concept pages use: **What it is**, **Why it exists**, **How it works**, **Trade-offs**, **Where to see it here**.

## Rules for content

1. **No number without a record.** A measured value appears only if an evidence entry backs it, and the sentence carries the conditions that matter (fabric version, load, sample count). If a value was measured on an older fabric, say so in the same sentence.
2. **Name the kind of evidence.** Simulation, tool output and board measurement are different things. Write "in simulation", "the timing report shows", "on the board".
3. **Nothing invented.** Addresses, register bits, pin names, commands and tool options come from the source files or the vendor documents. If something is unknown, write that it is unknown.
4. **Real code, short.** Quote the few lines that carry the idea, with the file named, and explain the choice they embody. Do not paste whole functions.
5. **Source references name a file and a symbol**, never a line number. Line numbers rot.
6. **Open issues stay visible.** A known defect is described as one, with what is and is not understood.
7. **Vendor material is linked, not copied.** Cite the document by title and number.

## Style

- Address the reader as "you". Describe what the hardware does in the present tense.
- Define a term the first time it appears on a page, even if another page defines it too.
- Prefer a table for anything with more than two attributes, a numbered list for anything done in order.
- Sentences carry one idea. If a sentence needs a semicolon, it is probably two sentences.
- Diagrams are text: Mermaid for structure and sequences, plain text or WaveDrom for timing. No binary images for anything that can be drawn as text.
- British spelling; SI units with a space (`20 ns`, `125 MHz`); hexadecimal addresses as `0x4000_0F00`.

## Adding a page

1. Write the page in the right directory with complete front matter. Name the file `<ID>-<short-name>.md`.
2. If the module was in `docs/modules/catalog.yaml`, remove its entry there.
3. Run `python3 tools/gen_docs.py` to refresh the indexes and the navigation block.
4. Run `python3 tools/check_content.py`. It must end with `OK`.
