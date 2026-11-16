#!/usr/bin/env python3
"""Export the pfdaq documentation into the Hugo source of afismail.com.

  tools/export_site.py <path to a checkout of the site repository>

The pages in this repository stay the single source. This script converts them to the site's
conventions and writes them into the site checkout; it never edits anything else there:

  content/projects/fpga-soc/microchip-polarfire-soc/pfdaq/
                                                  the project: its page (hand-written template below) and its parts,
                                                  each with articles and numbered labs
  content/resources/pfdaq-*.md                    explorer page, architecture reference, results
  static/pfdaq/                                   explorer, evidence records, architecture data

Re-running replaces exactly those paths, so the output is reproducible. The result is scanned with
.private-patterns when that file exists. Nothing is committed or pushed by this script.
"""
import json
import os
import re
import shutil
import sys

import yaml

import content as C
import gen_explorer as E

PROJECT_PATH = 'projects/fpga-soc/microchip-polarfire-soc/pfdaq'
PROJECT_URL = '/' + PROJECT_PATH + '/'
REPO = 'https://github.com/ismailalitech/pfdaq'
BLOB = REPO + '/blob/main/'
TREE = REPO + '/tree/main/'
DATE = '2026-11-15T12:00:00+01:00'
BOARD = 'PolarFire SoC Discovery Kit'
SIM = 'None (RTL simulation)'

# page id -> (topic path, sub-topic, weight, lab number or None, board, core)
PLACE = {
    'H02': ('fpga', 'streaming-datapaths', 10, 1, SIM, 'FPGA fabric'),
    'H04': ('fpga', 'streaming-datapaths', 20, 2, SIM, 'FPGA fabric'),
    'H05': ('fpga', 'streaming-datapaths', 30, 3, SIM, 'FPGA fabric'),
    'H06': ('fpga', 'streaming-datapaths', 40, 4, SIM, 'FPGA fabric'),
    'K03': ('fpga', 'streaming-datapaths', 50, None, None, None),
    'K09': ('fpga', 'streaming-datapaths', 60, None, None, None),
    'H01': ('fpga', 'registers-clocks-timing', 10, 5, SIM, 'FPGA fabric'),
    'H03': ('fpga', 'registers-clocks-timing', 20, 6, SIM, 'FPGA fabric'),
    'K02': ('fpga', 'registers-clocks-timing', 30, None, None, None),
    'K07': ('fpga', 'registers-clocks-timing', 40, None, None, None),
    'K08': ('fpga', 'registers-clocks-timing', 50, None, None, None),
    'H07': ('fpga', 'verification-and-instruments', 10, 7, SIM, 'FPGA fabric'),
    'H08': ('fpga', 'verification-and-instruments', 20, 8, SIM, 'FPGA fabric'),
    'H11': ('fpga', 'verification-and-instruments', 30, 9, SIM, 'FPGA fabric'),
    'H12': ('fpga', 'verification-and-instruments', 40, 1, SIM, 'FPGA fabric'),
    'C01': ('fpga', 'soc-integration', 10, None, None, None),
    'I04': ('fpga', 'soc-integration', 20, 10, BOARD, 'U54 (Linux)'),
    'F03': ('fpga', 'soc-integration', 30, 11, BOARD, 'U54 (Linux) and U54 hart 4'),
    'K01': ('fpga', 'soc-integration', 40, None, None, None),
    'K04': ('fpga', 'soc-integration', 50, None, None, None),
    'K05': ('fpga', 'soc-integration', 60, None, None, None),
    'K10': ('fpga', 'soc-integration', 70, None, None, None),
    'P02': ('microcontrollers', 'linux-and-rtos', 10, 12, BOARD, 'U54 harts 1 to 4'),
    'K06': ('microcontrollers', 'linux-and-rtos', 20, None, None, None),
    'K11': ('microcontrollers', 'linux-and-rtos', 30, None, None, None),
    'K12': ('microcontrollers', 'linux-and-rtos', 40, None, None, None),
    'S03': ('security', 'secure-boot', 10, 13, SIM, 'Development PC'),
    # boot and images
    'B01': ('embedded-linux', 'boot-and-images', 10, None, None, None),
    'B02': ('embedded-linux', 'boot-and-images', 20, 'auto', None, None),
    'B03': ('embedded-linux', 'boot-and-images', 30, 'auto', None, None),
    'B04': ('embedded-linux', 'boot-and-images', 40, 'auto', None, None),
    'B05': ('embedded-linux', 'boot-and-images', 50, 'auto', None, None),
    'B06': ('embedded-linux', 'boot-and-images', 60, None, None, None),
    'X03': ('embedded-linux', 'boot-and-images', 70, 'auto', None, None),
    # Linux to FPGA
    'I01': ('embedded-linux', 'linux-fpga-interfaces', 10, 'auto', None, None),
    'I02': ('embedded-linux', 'linux-fpga-interfaces', 20, 'auto', None, None),
    'I03': ('embedded-linux', 'linux-fpga-interfaces', 30, 'auto', None, None),
    'I05': ('embedded-linux', 'linux-fpga-interfaces', 40, 'auto', None, None),
    'I06': ('embedded-linux', 'linux-fpga-interfaces', 50, 'auto', None, None),
    'I07': ('embedded-linux', 'linux-fpga-interfaces', 60, 'auto', None, None),
    # fundamentals
    'F01': ('fpga', 'polarfire-soc-fundamentals', 10, None, None, None),
    'F02': ('fpga', 'polarfire-soc-fundamentals', 20, 'auto', None, None),
    'F04': ('fpga', 'polarfire-soc-fundamentals', 30, None, None, None),
    'F05': ('fpga', 'polarfire-soc-fundamentals', 40, None, None, None),
    'F07': ('fpga', 'polarfire-soc-fundamentals', 50, 1, 'None (Libero scratch project)', 'Development PC'),
    'H09': ('fpga', 'soc-integration', 90, None, None, None),
    'H10': ('fpga', 'registers-clocks-timing', 60, None, None, None),
    'X02': ('fpga', 'soc-integration', 85, 'auto', None, None),
    # Linux and an RTOS
    'M01': ('microcontrollers', 'linux-and-rtos', 5, None, None, None),
    'M02': ('microcontrollers', 'linux-and-rtos', 12, 'auto', None, None),
    'M03': ('microcontrollers', 'linux-and-rtos', 14, 'auto', None, None),
    'M04': ('microcontrollers', 'linux-and-rtos', 16, 'auto', None, None),
    'M05': ('microcontrollers', 'linux-and-rtos', 18, 'auto', None, None),
    'M07': ('microcontrollers', 'linux-and-rtos', 25, 'auto', None, None),
    'M06': ('microcontrollers', 'linux-and-rtos', 35, None, None, None),
    # performance and reliability
    'P01': ('fpga', 'performance-and-reliability', 10, 'auto', None, None),
    'P03': ('fpga', 'performance-and-reliability', 20, 'auto', None, None),
    'P04': ('fpga', 'performance-and-reliability', 30, 'auto', None, None),
    'P05': ('fpga', 'performance-and-reliability', 40, 'auto', None, None),
    'P06': ('fpga', 'performance-and-reliability', 50, 'auto', None, None),
    'X04': ('fpga', 'performance-and-reliability', 60, None, None, None),
    'P07': ('fpga', 'performance-and-reliability', 70, 'auto', None, None),
    'P08': ('fpga', 'performance-and-reliability', 80, 'auto', None, None),
    'I08': ('embedded-linux', 'linux-fpga-interfaces', 70, 'auto', None, None),
    'F06': ('embedded-linux', 'linux-fpga-interfaces', 80, 'auto', None, None),
    'M10': ('microcontrollers', 'linux-and-rtos', 50, 'auto', None, None),
}
CORE_BY_TRACK = {'boot': 'E51 and U54', 'fundamentals': 'PolarFire SoC', 'integration': 'U54 (Linux)',
                 'multicore': 'U54 (Linux) and U54 hart 4', 'performance': 'U54 (Linux) and FPGA fabric',
                 'system': 'U54 (Linux) and FPGA fabric'}
TOPICS = {}
SUBTOPICS = {
    'polarfire-soc-fundamentals': ('The chip and the board', 'The chip and the board, the memory map, clocks and resets, and the tool flow.', 10),
    'streaming-datapaths': ('Streaming datapaths', 'Frames, CRC and filters on AXI4-Stream, a source that cannot be paused, and what a DMA needs from a stream.', 20),
    'registers-clocks-timing': ('Registers, clocks and timing', 'Register blocks software can trust, crossing between clock domains, and constraints that say exactly what they mean.', 30),
    'verification-and-instruments': ('Verification and instruments', 'Checking hardware with an independent model, and building measuring instruments into the fabric.', 40),
    'soc-integration': ('SoC integration', 'Joining FPGA logic to processors: DMA, interrupts and memory, with the failures met on a real board.', 50),
    'boot-and-images': ('Boot and images', 'From power-on to login: the boot monitor, U-Boot, the payload, the Yocto layer, the device tree and how to update a running board.', 60),
    'linux-fpga-interfaces': ('Linux and the FPGA', 'Registers, interrupts, buffers and drivers: how a Linux program and the kernel work with logic in the fabric.', 70),
    'linux-and-rtos': ('Linux and an RTOS on one chip', 'Running Linux and a real-time supervisor side by side, and measuring how each answers an interrupt.', 80),
    'performance-and-reliability': ('Performance and reliability', 'Where throughput goes, fault injection, deadlines, abort, soak and reboot testing, and the validation report.', 90),
    'secure-boot': ('Secure boot, offline', 'How a signed boot chain works, with a model and the real signing format, run on a PC. Nothing is flashed.', 100),
}
LEVEL = {1: 'Beginner', 2: 'Beginner', 3: 'Intermediate', 4: 'Advanced', 5: 'Advanced'}
STATUS_TEXT = {'validated': 'validated', 'validated-old': 'validated on an earlier fabric version', 'simulated': 'simulated',
               'experimental': 'experimental', 'planned': 'planned'}
MERMAID = {
    'F03': """| Line from the fabric | Source | PLIC source | Enabled for |
|---|---|---|---|
| F2M[1] | Latency channel 0 | 119 | Linux, harts 1 to 3 |
| F2M[2] | DMA controller | 120 | Linux, harts 1 to 3 |
| F2M[5] | Latency channel 1 | 123 | Hart 4, machine mode |
""",
    'C01': """1. The processor writes the descriptor (destination, length, ready flag) into memory.
2. It points the DMA controller at the descriptor and enables the interrupt.
3. It starts the data source.
4. For every bus word, the source sends data to the DMA controller, which writes it to memory.
5. The DMA controller clears the descriptor's ready flag.
6. It raises the completion interrupt.
7. The processor reads the data from memory.
""",
}


def lab_numbers():
    """Labs are numbered 1, 2, 3 ... in the order of the project: by part, then by position inside the part."""
    order = list(SUBTOPICS)
    labs = [(order.index(v[1]), v[2], pid) for pid, v in PLACE.items() if v[3]]
    return {pid: n for n, (_p, _w, pid) in enumerate(sorted(labs), 1)}


LAB_NO = lab_numbers()


class Site:
    def __init__(self, root):
        self.root = root
        self.urls = {}        # page id -> url
        self.files = {}       # repo file -> page id
        for pid, (topic, sub, _w, _l, _b, _c) in PLACE.items():
            self.urls[pid] = PROJECT_URL + sub + '/'
        self.urls['X01'] = PROJECT_URL
        self.pages = {p['id']: p for p in C.load_pages() if 'id' in p}
        for pid, p in self.pages.items():
            self.files[p['file']] = pid
            if pid in PLACE:
                topic, sub = PLACE[pid][0], PLACE[pid][1]
                self.urls[pid] = PROJECT_URL + '%s/%s/' % (sub, self.slug(p))

    @staticmethod
    def slug(p):
        return os.path.basename(p['file'])[:-3].lower()

    def url(self, pid):
        return self.urls[pid]


FIXED = {
    'docs/architecture.md': '/resources/pfdaq-architecture/',
    'docs/validation-plan.md': '/resources/pfdaq-results/',
    'docs/system/README.md': '/resources/pfdaq-explorer/',
    'docs/modules/README.md': PROJECT_URL,
    'docs/learning-path.md': PROJECT_URL,
    'docs/system/architecture.yaml': '/pfdaq/architecture.yaml',
}


def repo_link(rp):
    """URL of a path in the public pfdaq repository, or None for paths that are not published."""
    if rp.startswith(('docs/platform', 'docs/vendor', 'docs/source-index', 'docs/system-guide', 'logs/', 'tools/private')) or not rp.startswith(
            ('hw/', 'firmware/', 'yocto/', 'tests/', 'tools/', 'docs/', 'experiments/')):
        return None
    full = os.path.join(C.ROOT, rp)
    if os.path.isdir(full):
        return TREE + rp
    return BLOB + rp if os.path.isfile(full) else None


def rewrite_links(text, src_file, site):
    srcdir = os.path.dirname(src_file)

    def fix(m):
        label, target = m.group(1), m.group(2)
        if re.match(r'[a-z]+:', target) or target.startswith('#'):
            return m.group(0)
        path, _, frag = target.partition('#')
        rp = os.path.normpath(os.path.join(srcdir, path))
        frag = ('#' + frag) if frag else ''
        if rp in site.files and (site.files[rp] in PLACE or site.files[rp] == 'X01'):
            return '[%s](%s%s)' % (label, site.url(site.files[rp]), frag)
        if rp == 'docs/system/README.md':
            return '[%s](/pfdaq/explorer/%s)' % (label, frag)
        if rp in FIXED:
            return '[%s](%s%s)' % (label, FIXED[rp], frag if rp != 'docs/system/README.md' else '')
        if rp.startswith('evidence/'):
            return '[%s](/pfdaq/%s)' % (label, rp)
        link = repo_link(rp)
        return '[%s](%s%s)' % (label, link, frag) if link else label
    def fix_image(m):
        alt, target = m.group(1), m.group(2)
        if re.match(r'[a-z]+:', target):
            return m.group(0)
        rp = os.path.normpath(os.path.join(srcdir, target))
        if rp.startswith('evidence/'):
            url = '/pfdaq/' + rp
        elif rp.startswith('docs/diagrams/'):
            url = '/pfdaq/diagrams/' + os.path.basename(rp)
        else:
            return m.group(0)
        return '[![%s](%s)](%s)' % (alt, url, url)
    out = re.sub(r'(?<!!)\[([^\]]+)\]\(([^)\s]+)\)', fix, text)
    return re.sub(r'!\[([^\]]*)\]\(([^)\s]+)\)', fix_image, out)


def body_of(page, site):
    text = page['body']
    text = re.sub(r'<!-- nav:begin.*?<!-- nav:end -->\n?', '', text, flags=re.S)
    text = re.sub(r'<!--.*?-->\n?', '', text, flags=re.S)
    text = re.sub(r'\A\s*# .*\n', '', text, count=1)
    text = re.sub(r'\n---\s*\Z', '\n', text.rstrip() + '\n')
    if page['id'] in MERMAID:
        text = re.sub(r'```mermaid\n.*?```\n', lambda m: MERMAID[page['id']], text, flags=re.S)
    text = rewrite_links(text, page['file'], site)
    return text.strip() + '\n'


def at_a_glance(page, site, evidence):
    rows = [('Level', LEVEL[page['level']]), ('How to run', C.REPRO[page['repro']]), ('Status', STATUS_TEXT[page['status']])]
    if page.get('needs'):
        shown = ['[%s %s](%s)' % (i, site.pages[i]['title'], site.url(i)) for i in page['needs']
                 if i in site.pages and (i in PLACE or i == 'X01')]
        if shown:
            rows.append(('Read first', '; '.join(shown)))
    ids = (page.get('hardware') or []) + (page.get('software') or [])
    if ids:
        nodes = {n['id']: n for n in C.load_architecture()['nodes']}
        rows.append(('In the system', ' · '.join('[%s](/pfdaq/explorer/#%s)' % (nodes[i]['title'], i) for i in ids)))
    src = C.norm_sources(page.get('sources'))
    if src:
        rows.append(('Source files', '; '.join('[`%s`](%s)%s' % (s['path'], BLOB + s['path'], (': ' + ', '.join('`%s`' % x for x in s['symbols'])) if s.get('symbols') else '') for s in src)))
    if page.get('evidence'):
        rows.append(('Evidence', '; '.join('[%s](/pfdaq/%s) (%s%s)' % (
            evidence[i]['title'], evidence[i]['path'], evidence[i]['class'], C.fabric_label(evidence[i])) for i in page['evidence'])))
    if page.get('related'):
        shown = ['[%s %s](%s)' % (i, site.pages[i]['title'], site.url(i)) for i in page['related']
                 if i in site.pages and (i in PLACE or i == 'X01')]
        if shown:
            rows.append(('Related', '; '.join(shown)))
    if page['repro'] in ('sim', 'base'):
        rows.append(('Code', 'The commands on this page run from the [pfdaq repository](%s).' % REPO))
    out = ['## At a glance', '', '| | |', '|---|---|'] + ['| **%s** | %s |' % r for r in rows]
    return '\n'.join(out) + '\n'


def toml_str(s):
    return json.dumps(s, ensure_ascii=False)


def page_file(page, site, evidence):
    pid = page['id']
    topic, sub, weight, lab, board, core = PLACE[pid]
    if lab == 'auto':
        board, core = BOARD, CORE_BY_TRACK.get(page['track'], 'PolarFire SoC')
    if lab:
        lab = LAB_NO[pid]
    fm = ['+++', 'title = %s' % toml_str(page['title']), "date = '%s'" % DATE, 'draft = false',
          'description = %s' % toml_str(page['summary']), 'weight = %d' % weight,
          'aliases = [%s]' % toml_str('/topics/%s/%s/%s/' % (topic, sub, Site.slug(page))), '', '[params]']
    if lab:
        fm += ['  lab = %d' % lab, '  board = %s' % toml_str(board), '  core = %s' % toml_str(core),
               '  level = %s' % toml_str(LEVEL[page['level']])]
    fm.append('+++')
    banner = ''
    if page['status'] == 'experimental':
        banner = ('> **Experimental.** The code on this page has been compiled and reviewed, but some steps have not yet been '
                  'run on the board. The page says which.\n\n')
    return '\n'.join(fm) + '\n\n' + banner + body_of(page, site) + '\n' + at_a_glance(page, site, evidence)


WORK = """+++
title = "Instrumented stream acquisition on PolarFire SoC"
description = "An FPGA data source, a DMA path into DDR, Linux on three RISC-V cores and a real-time supervisor on the fourth, built to be measured and made to fail on purpose."
date = @DATE@
draft = false
weight = 20
aliases = ["/work/pfdaq-polarfire-soc/"]

[params]
  project = true
  platform = "Microchip PolarFire SoC"
  problem = "A data path that crosses an FPGA, a DMA, an operating system that is not real-time and a second processor is easy to make work once and hard to trust. Every layer can hide a fault the others cannot see."
  role = "Designed and built the complete system: the FPGA logic and its testbenches, the Linux image, the real-time firmware, the capture and measurement tools, and the board test suites."
  result = "27 of 27 functional tests pass from a freshly written SD card; injected faults are caught in every payload mode; the supervisor answers an interrupt in 0.5 microseconds typically, against 78 microseconds for a Linux user-space program."
  hardware = "Microchip PolarFire SoC Discovery Kit (MPFS095T): one E51 and four U54 RISC-V cores, FPGA fabric, 1 GB DDR4"
  tools = "Libero SoC 2026.1, Icarus Verilog, Yocto (Scarthgap), FreeRTOS, Python"
  action = "/resources/pfdaq-explorer/"
  action_text = "Explore the architecture"
  guide = "@GUIDE@"
  toc = true
+++

## Problem

Test data is easy to generate and hard to trust once it has travelled through hardware, a DMA controller, a cache, a
kernel and a user program. When the end result is wrong, any layer might be at fault. I wanted a system in which every
layer could be checked independently, made to fail on purpose, and measured, so that a claim about it could be backed
by a record.

## Hardware and constraints

The board is the PolarFire SoC Discovery Kit. Its chip combines five RISC-V cores with an FPGA, which allows each job
to run where it fits.

| Core | Runs | Why there |
|---|---|---|
| E51 | Hart Software Services (boot monitor) | starts everything else |
| U54, three cores | Linux 6.18, built with Yocto | configuration, capture, verification, network |
| U54, fourth core | A supervisor on FreeRTOS, or the same source on bare metal | must keep its timing whatever Linux does |

Constraints that shaped the design:

- Two software contexts is the vendor's maximum, so FreeRTOS and bare metal are alternative builds of the same
  supervisor, never present together.
- The boot flash holds the vendor's boot monitor. Nothing in this project reprograms it; only the fabric is programmed.
- The DMA controller has rules that its documentation does not state. They were found by testing and are collected in
  [DMA stream descriptors](@I04@).

The fabric half of the system, as the vendor's tool shows it: the project's own block, `pfdaq_core`, feeds the DMA
controller over an AXI4-Stream link and raises two latency interrupts. The other blocks are the reference design's.

[![The FIC_0_PERIPHERALS canvas in Libero SoC: pfdaq_core, the DMA controller, its initiator and target interconnects and the on-chip SRAM](/pfdaq/evidence/v0.5.1/build/libero-fic0-peripherals.png)](/pfdaq/evidence/v0.5.1/build/libero-fic0-peripherals.png)

## My contribution

- **FPGA logic.** A stream source that produces framed data (header, sequence number, timestamp, payload, CRC-32) two
  words per clock, with counter, pseudo-random and FIR payloads, fault injection, a real-time mode with credits and
  counted drops, and an abort that never leaves the DMA waiting. A two-channel latency unit that measures how long
  software takes to answer an interrupt, with 20 ns resolution.
- **Software.** A Linux capture tool that verifies every frame against a software model, a kernel driver, and a
  supervisor that checks Linux against the fabric's own counters.
- **Verification.** Testbenches that only record, judged by independent Python models; a board suite with 27 functional
  tests, run from a freshly written card.
- **Measurement.** The interrupt-to-acknowledge latency at five software levels, measured by the fabric itself.

The parts of the project, each with its notes and numbered labs, are listed at the top of this page, in the order a
reader would work through them.

## Architecture

One burst, from start to verified:

1. The Linux capture tool writes a *descriptor* into memory (where the data goes, how many bytes) and tells the DMA
   controller where it is.
2. It sets the frame length and count on the stream source and starts it.
3. The source sends frames as a 64-bit AXI4-Stream, two words per 125 MHz clock.
4. The DMA writes the stream into DDR; when the byte count is reached it raises an interrupt.
5. Linux copies the burst out and checks every frame.
6. It reports to the supervisor over RPMsg. The supervisor enforces a deadline and compares the report with the
   fabric's own sequence counter, which it reads directly.

The [architecture explorer](/pfdaq/explorer/) shows every component and connection, and plays this walk step by step.
The full register maps, frame format and protocols are in the [architecture reference](/resources/pfdaq-architecture/).

## Verification

Three kinds of evidence, kept apart on purpose:

- **Simulation.** 20 stream scenarios and 4 latency phases, each judged by a model written separately from the RTL.
- **Build.** Timing met in all corners, all 64 pins placed, 26 % of the fabric used.
- **Board.** The full suite on a freshly written SD card, 30 warm reboots, and the latency measurements.

Every number is traced to a record with the command and the conditions that produced it:
[results and evidence](/resources/pfdaq-results/).

## Results and limits

| Question | Answer |
|---|---|
| Does it work from a clean start? | 27 of 27 functional tests pass from a freshly written card |
| Does it come up every time? | 30 of 30 warm reboots, both systems up |
| Are injected faults caught? | Every corrupted CRC, skipped sequence number and flipped bit, in all three payload types |
| Hardware path speed | 931 to 947 MB/s into memory (measured on an earlier fabric version) |
| Verified path speed | 29.6 MB/s for counter payload: one Linux core copying and checking |
| Interrupt response, supervisor core | 0.5 microseconds typical, 3.1 microseconds worst seen, none missed in 600,000 |
| Interrupt response, Linux user space | 78 microseconds typical, up to 5.1 ms |

Limits, stated plainly:

- **Odd frame lengths in multi-frame bursts corrupt data.** The stream is correct in simulation and timing is met; the
  cause is not found. The capture tool refuses the combination. See [the case study](@K10@).
- **Linux cannot restart the supervisor core.** It can stop it; the boot monitor rejects the restart. Recovery is a
  reboot. See [the case study](@K11@).
- The ten-minute, 17.8 GB soak was run on an earlier fabric version and has not been repeated on the final one.
- Everything was measured on one board.
- The code is public: [github.com/ismailalitech/pfdaq](https://github.com/ismailalitech/pfdaq). The labs run from it.
"""

EXPLORER_PAGE = """+++
title = "pfdaq architecture explorer"
description = "Every component and connection of the pfdaq system on PolarFire SoC: select a block, a bus or an interrupt line, or play a walk step by step."
weight = 50

[params]
  topics = ["Tool"]
  toc = true
+++

An interactive map of the whole system. It has two views, one for the hardware and data paths and one for the software
and who runs where. Select a box to see what it does, what it connects to and which labs explain it. Select a line to
see what it carries. Three walks play a burst, an interrupt and the boot sequence one step at a time.

**[Open the explorer](/pfdaq/explorer/)**

## What it shows

- **Components.** The cores, the interrupt controller, the two buses between the processor subsystem and the fabric, the
  fabric blocks, the memory regions and the software, with the address, version or clock where it matters.
- **Connections.** Register buses, memory buses, the data stream, interrupts, messages and boot hand-over, each drawn
  differently so a line can be read without a legend.
- **Walks.** One burst from start to verified; one latency measurement on the supervisor core; boot from power-on to
  both systems running.

## Where the data comes from

Everything on the map comes from one file, [`architecture.yaml`](/pfdaq/architecture.yaml). The same file produces the
component descriptions in the labs, so the map and the text cannot disagree. Each fact carries the kind of evidence
behind it: simulation, tool output or measurement on the board.

## Related

- [Architecture reference](/resources/pfdaq-architecture/): register maps, frame format, DMA rules, protocols.
- [Results and evidence](/resources/pfdaq-results/): every test, its method, its result and its record.
- [The project page](/projects/fpga-soc/microchip-polarfire-soc/pfdaq/).
"""


def resource_page(title, desc, weight, body):
    return '+++\ntitle = %s\ndescription = %s\nweight = %d\n\n[params]\n  topics = ["Reference"]\n  toc = true\n+++\n\n%s' % (
        toml_str(title), toml_str(desc), weight, body)


def convert_doc(path, site):
    with open(os.path.join(C.ROOT, path)) as f:
        text = f.read()
    text = re.sub(r'\A# .*\n', '', text, count=1)
    return rewrite_links(text, path, site).strip() + '\n'


def results_extra(evidence):
    out = ['\n## Evidence records\n',
           'Each record below can be downloaded. The command and conditions are those under which it was produced; a number on '
           'any page of this site is quoted from one of these.\n',
           '| Record | Kind | Fabric | Date | Command | Conditions |', '|---|---|---|---|---|---|']
    for e in evidence.values():
        out.append('| [%s](/pfdaq/%s) | %s | %s | %s | %s | %s |' % (
            e['title'].replace('|', '/'), e['path'], e['class'], ('v' + e['fabric']) if e['fabric'] != 'n/a' else 'none', e['date'],
            ('`%s`' % e['command']).replace('|', '/'), (e.get('conditions') or '').replace('|', '/')))
    return '\n'.join(out) + '\n'


def write(path, text, written):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(text)
    written.append(path)


def main():
    if len(sys.argv) != 2 or not os.path.isdir(os.path.join(sys.argv[1], 'content')):
        sys.exit(__doc__)
    root = os.path.abspath(sys.argv[1])
    site = Site(root)
    evidence = C.load_evidence()
    arch = C.load_architecture()
    layout = E.load_layout()
    mods = C.all_modules()
    written = []

    # remove exactly what this script owns, then regenerate it
    project_dir = os.path.join(root, 'content', PROJECT_PATH)
    owned = [project_dir, os.path.join(root, 'static/pfdaq')]
    owned += [os.path.join(root, 'content/resources/pfdaq-%s.md' % n) for n in ('explorer', 'architecture', 'results')]
    # where this material lived before it was organised by project; the pages keep their old addresses as aliases
    owned += [os.path.join(root, 'content/work/pfdaq-polarfire-soc.md'), os.path.join(root, 'content/topics/embedded-linux')]
    for sub in SUBTOPICS:
        old = PLACE_TOPIC(sub)
        if old and sub != 'secure-boot':      # the site's own Secure boot folder keeps its index page
            owned.append(os.path.join(root, 'content/topics', old, sub))
    owned.append(os.path.join(root, 'content/topics/security/secure-boot/s03-signed-boot-chain.md'))
    for p in owned:
        if os.path.isdir(p):
            shutil.rmtree(p)
        elif os.path.isfile(p):
            os.remove(p)

    # the project page and its parts
    write(os.path.join(project_dir, '_index.md'),
          WORK.replace('@DATE@', "'%s'" % DATE).replace('@I04@', site.url('I04')).replace('@K10@', site.url('K10')).replace('@K11@', site.url('K11')).replace('@GUIDE@', site.url('X03')), written)
    for sub, (title, desc, weight) in SUBTOPICS.items():
        old = PLACE_TOPIC(sub)
        aliases = ['/topics/%s/%s/' % (old, sub)] if old else []
        fm = ['+++', 'title = %s' % toml_str(title), 'description = %s' % toml_str(desc), 'weight = %d' % weight]
        if aliases:
            fm.append('aliases = [%s]' % ', '.join(toml_str(a) for a in aliases))
        write(os.path.join(project_dir, sub, '_index.md'), '\n'.join(fm + ['+++', '']), written)
    for pid in PLACE:
        p = site.pages[pid]
        sub = PLACE[pid][1]
        write(os.path.join(project_dir, sub, site.slug(p) + '.md'), page_file(p, site, evidence), written)

    write(os.path.join(root, 'content/resources/pfdaq-explorer.md'), EXPLORER_PAGE, written)
    write(os.path.join(root, 'content/resources/pfdaq-architecture.md'),
          resource_page('pfdaq architecture reference', 'Register maps, frame format, DMA rules, the RPMsg protocol, boot flow and risks of the pfdaq system on PolarFire SoC.', 60,
                        'The system as built, with the kind of evidence behind each statement (**B** board, **S** simulation, **BUILD** tool output, **V** vendor document or source, **I** inference, **U** unresolved).\n\n' + convert_doc('docs/architecture.md', site)), written)
    write(os.path.join(root, 'content/resources/pfdaq-results.md'),
          resource_page('pfdaq results and evidence', 'Every test of the pfdaq system, its method, result and record, and the failures found along the way.', 70,
                        convert_doc('docs/validation-plan.md', site) + results_extra(evidence)), written)

    # static: explorer, architecture data, evidence records, the experiment source
    s = {'root': BLOB, 'url': site.url, 'back': PROJECT_URL, 'system': PROJECT_URL, 'yaml': '/pfdaq/architecture.yaml', 'home': '/', 'projects': '/projects/'}
    write(os.path.join(root, 'static/pfdaq/explorer/index.html'), E.build_html(arch, layout, mods, s), written)
    shutil.copy(os.path.join(C.DOCS, 'system', 'architecture.yaml'), os.path.join(root, 'static/pfdaq/architecture.yaml'))
    for d in ('evidence',):
        if os.path.isdir(os.path.join(C.ROOT, d)):
            shutil.copytree(os.path.join(C.ROOT, d), os.path.join(root, 'static/pfdaq', d))
    os.makedirs(os.path.join(root, 'static/pfdaq/diagrams'), exist_ok=True)
    for f in sorted(os.listdir(os.path.join(C.DOCS, 'diagrams'))):
        if f.endswith('.svg'):
            shutil.copy(os.path.join(C.DOCS, 'diagrams', f), os.path.join(root, 'static/pfdaq/diagrams', f))
    written.append(os.path.join(root, 'static/pfdaq'))

    # scan what was written for private patterns
    pat_file = os.path.join(C.ROOT, '.private-patterns')
    hits = 0
    if os.path.isfile(pat_file):
        pats = [l.rstrip('\n').split('\t')[0] for l in open(pat_file) if l.strip()]
        for base in written:
            for dp, _dn, fns in os.walk(base) if os.path.isdir(base) else [(os.path.dirname(base), [], [os.path.basename(base)])]:
                for fn in fns:
                    fp = os.path.join(dp, fn)
                    try:
                        text = open(fp, errors='strict').read()
                    except UnicodeDecodeError:
                        continue
                    for i, line in enumerate(text.split('\n'), 1):
                        for pt in pats:
                            if re.search(pt, line):
                                hits += 1
                                print('PRIVATE %s:%d /%s/' % (os.path.relpath(fp, root), i, pt[:24]))
    print('%d pages and resources written; %d private hit(s)' % (len([w for w in written if w.endswith('.md')]), hits))
    return 1 if hits else 0


def PLACE_TOPIC(sub):
    for v in PLACE.values():
        if v[1] == sub:
            return v[0]


if __name__ == '__main__':
    sys.exit(main())
