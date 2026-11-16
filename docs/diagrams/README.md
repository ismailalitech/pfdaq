# Animated diagrams

Every figure that plays (boxes and arrows lighting up in turn, waveforms with callouts, a console with callouts, a recorded
session played back, a screenshot with callouts) is described in YAML in this folder and turned into an SVG by `tools/gen_diagrams.py`.
Any `*.yaml` file here is read; each diagram has a unique `id`; the SVG has the same name. Run
`python3 tools/gen_diagrams.py` after editing, and `python3 tools/check_content.py` before you finish.
In a page (the site makes the picture click-to-enlarge):

```text
![what it shows, in a sentence](../diagrams/ID.svg)
```

Common keys: `id`, `alt` (what the picture shows, for readers who cannot see it), `width` (900 is good), `height`,
`step` (seconds per callout, 3 to 3.6), `type`.

## `type` omitted: boxes and arrows (flow)
Nodes are placed by hand in pixels; edges join sides of nodes; `steps` is the order boxes and arrows light up
(node ids, and edge ids `e0`, `e1` ... in the order the edges are listed, or an explicit `id`).
```yaml
- id: example-flow
  alt: "..."
  width: 800
  height: 220
  nodes:
    a: {x: 20,  y: 40, w: 220, h: 66, kind: hart,   title: "Linux", sub: "first line\nsecond line"}
    b: {x: 300, y: 40, w: 220, h: 66, kind: fabric, title: "Fabric"}
  edges:
    - {from: a.right, to: b.left, label: "register write"}      # sides: top bottom left right, e.g. a.right@0.3
  steps: [a, e0, b]
  notes:
    - {x: 20, y: 150, text: "free text"}
```
`kind`: plain, start, hart, mss, fabric, mem, sw, good, bad. Text must fit the box (the generator refuses wide text):
about 6.4 pixels per character at the small size, 7.8 for the bold title. Edge keys: `label`, `dashed`, `kind`, `bulge`, `mid`.

## `type: terminal`: recorded console output with numbered callouts
```yaml
- id: example-terminal
  type: terminal
  title: "board console"
  alt: "..."
  width: 900
  height: 330          # at least 58 + 20 * lines + 16 + 70
  step: 3.4
  lines:
    - "$ pfdaq_lat -c 1 -p 200 -n 100000"        # a line starting "$ " or "# " is a command
    - '{"pass": true, "events": 100000}'
  callouts:
    - {lines: [0],    title: "The command", text: "one sentence, at most about 110 characters"}
    - {lines: [1, 1], title: "The result",  text: "..."}
```
Lines are limited to about 110 characters. Use only text that a real run printed (copy it from a record in `evidence/`
or from a run you made now); shorten with `...` only when you say so in the caption.

## `type: replay`: a recorded terminal session, played back at its recorded times
Two or three stacked terminal panes whose lines appear at the times a real run printed them, with numbered callouts.
```yaml
- id: example-replay
  type: replay
  alt: "..."
  width: 900
  rows: 8                         # visible lines per pane (default 8); a pane can set its own `rows`
  max_gap: 1.5                    # seconds; any longer pause in the recording is shortened to this (default 1.5)
  recording: recordings/example.jsonl     # relative to docs/diagrams
  panes:                          # 2 or 3, pane 0 on top
    - {title: "Linux CPU2: operator dashboard (ssh)", rows: 6}
    - {title: "Linux CPU0 and CPU1: acquisition and analysis logs (ssh)"}
    - {title: "hart 4 console (UART)"}
  callouts:                       # in order of t; t is in recorded seconds
    - {t: 12.0, title: "Normal operation", text: "one sentence, at most about 110 characters", panes: [0]}
```
The recording is one JSON object per line, written by the capture script, sorted by `t`:
```json
{"t": 12.04, "pane": 0, "text": "t=12 frames=12288 bad=0 drops=0 rate=31 MB/s hart4=ARMED"}
```
`t` is seconds since the start of the run (one clock for all panes), `pane` is the index in `panes`, `text` is one
line without colour codes or tabs. A line starting `$ ` or `# ` is drawn as a command, as in `terminal`.
What the generator does:
- **Time compression.** The gap between two lines, across all panes, is cut to `max_gap` when it is longer. The
  figure says so ("gaps over 1.5 s are shortened") and shows a clock with the recorded time (mm:ss) next to the
  playback; the clock runs fast through a shortened gap. Playback restarts after 3 s (`hold`).
- **Scrolling.** A pane shows its last `rows` lines; a new line pushes the oldest one out at the top.
- **Callouts.** Callout `t` is a recorded time. At that moment (compressed) the numbered caption appears below, and
  it stays until the next callout. With `panes: [..]` the pane is outlined with the number in its title bar, and the
  last line it had printed at `t` is highlighted and scrolls with the text.
- **Checks (the generator stops with a message).** Wrong pane count, a pane index that does not exist, `t` not sorted,
  a callout outside the recording or out of order, control characters in a line, a line longer than about 109
  characters at width 900 (`clip: true` cuts it with an ellipsis instead), a height smaller than needed (leave
  `height` out to get the exact one), more than 600 lines in one recording (each line has its own animation; about 200
  kB of SVG at 400 lines).
- With reduced motion the figure shows the final state of the panes and no callouts, so keep the meaning in `alt`.
Keep recordings that a real run wrote under `docs/diagrams/recordings/`; do not edit lines by hand.

## `type: wave`: waveforms, with numbered callouts
```yaml
- id: example-wave
  type: wave
  alt: "..."
  width: 900
  height: 330          # at least 44 + 40 * signals + 12 + 74
  length: 400          # time span in the unit of the numbers below (e.g. nanoseconds)
  ticks: [[0, "0 ns"], [200, "200 ns"]]
  step: 3.4
  signals:
    - {name: TVALID, kind: fabric, values: [[0, 0], [20, 1], [140, 0]]}               # [time, 0 or 1] at each change
    - {name: TDATA, bus: true, kind: hart, values: [[0, "-"], [20, "0x01"], [60, "0x02"]]}   # [time, label]
  callouts:
    - {t0: 20, t1: 60, title: "One beat", text: "TVALID and TREADY high: the word transfers."}
```
Take the values from a real simulation run (for example `make -C hw sim-stream`, trace in `build/sim/`), by a small
script that extracts a window, so the figure is the simulation and not a drawing of what you expect.

## `type: annotated`: a screenshot with numbered callouts
`image:` is a path from the repository root (for example `evidence/v0.5.1/build/libero-fic0-peripherals.png`);
`callouts: [{box: [x, y, w, h], title, text}]` with the box in the image's own pixels; `height` = image height scaled to `width`
plus 78.

## `type: timeline`: lanes on a time axis
See `burst-pipeline` in `diagrams.yaml`.
