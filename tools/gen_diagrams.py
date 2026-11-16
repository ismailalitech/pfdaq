#!/usr/bin/env python3
"""Generate the animated SVG diagrams of the documentation from docs/diagrams/diagrams.yaml.

Every diagram is boxes and arrows (or lanes on a time axis) drawn on a small dark card. While it plays, the
boxes and arrows light up one after the other in the order given by `steps`, so a reader sees the sequence
and not only the picture. The files are self-contained SVG (no script), animate with CSS, and show the plain
picture when the reader prefers reduced motion. The generated files are committed;
`tools/check_content.py` fails if they are out of date.

usage: tools/gen_diagrams.py [--check]
"""
import os, sys, re
import yaml
from xml.sax.saxutils import escape

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIR = os.path.join(ROOT, 'docs', 'diagrams')
SPEC = os.path.join(DIR, 'diagrams.yaml')

FONT = 'Inter, system-ui, -apple-system, "Segoe UI", Roboto, sans-serif'
# kind -> (fill, stroke)
KINDS = {
    'plain': ('#1e2530', '#6b7b8c'),
    'start': ('#2a3140', '#c9d3dd'),
    'hart':  ('#1b3047', '#5aa0d6'),
    'mss':   ('#232c44', '#8da6d4'),
    'fabric': ('#1a3626', '#5fbf84'),
    'mem':   ('#352f1c', '#c0a95f'),
    'sw':    ('#2f2447', '#b08fe0'),
    'good':  ('#17382f', '#4fd1a5'),
    'bad':   ('#43201f', '#ef6f6c'),
}
BG, FG, MUTED, ACCENT = '#12171d', '#e6ecf1', '#9aa8b5', '#ffd166'
STEP = 1.3      # seconds per step
HOLD = 2.0      # pause at the end of a cycle


def esc(s):
    return escape(str(s))


def text_w(s, size):
    return 0.56 * size * len(s)


class Diagram:
    def __init__(self, d):
        self.d = d
        self.id = d['id']
        self.w = d.get('width', 780)
        self.h = d.get('height', 0) if d.get('type') == 'replay' else d['height']
        self.out = []
        self.names = []     # animation targets in order
        self.step = d.get('step', STEP)
        self.css_extra = ''     # extra CSS (replay figures)
        self.defs_extra = ''    # extra <defs> content (replay figures)

    # ---------- geometry
    def port(self, ref):
        m = re.match(r'^([\w-]+)\.(top|bottom|left|right)(?:@([0-9.]+))?$', ref)
        if not m:
            raise SystemExit('%s: bad port %r' % (self.id, ref))
        n = self.nodes[m.group(1)]
        f = float(m.group(3)) if m.group(3) else 0.5
        x, y, w, h = n['x'], n['y'], n['w'], n['h']
        side = m.group(2)
        if side == 'top':
            return (x + w * f, y), side
        if side == 'bottom':
            return (x + w * f, y + h), side
        if side == 'left':
            return (x, y + h * f), side
        return (x + w, y + h * f), side

    def route(self, e):
        (x1, y1), s1 = self.port(e['from'])
        (x2, y2), s2 = self.port(e['to'])
        horiz = lambda s: s in ('left', 'right')
        opp = {('right', 'left'), ('left', 'right'), ('top', 'bottom'), ('bottom', 'top')}
        pts = [(x1, y1)]
        if (s1, s2) in opp:
            if horiz(s1):
                if abs(y1 - y2) > 0.5:
                    mx = e.get('mid', (x1 + x2) / 2)
                    pts += [(mx, y1), (mx, y2)]
            else:
                if abs(x1 - x2) > 0.5:
                    my = e.get('mid', (y1 + y2) / 2)
                    pts += [(x1, my), (x2, my)]
        elif s1 == s2:
            b = e.get('bulge', 26)
            if s1 == 'right':
                xo = max(x1, x2) + b; pts += [(xo, y1), (xo, y2)]
            elif s1 == 'left':
                xo = min(x1, x2) - b; pts += [(xo, y1), (xo, y2)]
            elif s1 == 'bottom':
                yo = max(y1, y2) + b; pts += [(x1, yo), (x2, yo)]
            else:
                yo = min(y1, y2) - b; pts += [(x1, yo), (x2, yo)]
        else:
            pts += [(x2, y1)] if horiz(s1) else [(x1, y2)]
        pts.append((x2, y2))
        return pts

    # ---------- drawing
    def add(self, s):
        self.out.append(s)

    def node_svg(self, nid, n):
        fill, stroke = KINDS[n.get('kind', 'plain')]
        x, y, w, h = n['x'], n['y'], n['w'], n['h']
        title = n['title']
        subs = n.get('sub', '')
        subs = subs.split('\n') if subs else []
        lines = [(title, 14, 'bold', FG)] + [(t, 11.5, 'normal', MUTED) for t in subs]
        for t, sz, _wt, _c in lines:
            if text_w(t, sz) > w - 10:
                raise SystemExit('%s: text too wide for box %s: %r (%.0f > %d)' % (self.id, nid, t, text_w(t, sz), w - 10))
        lh = 17
        total = lh * len(lines)
        ty = y + (h - total) / 2 + 13
        self.add('<g id="%s">' % esc(nid))
        self.add('<rect x="%g" y="%g" width="%g" height="%g" rx="9" fill="%s" stroke="%s" stroke-width="1.5"/>' % (x, y, w, h, fill, stroke))
        for i, (t, sz, wt, c) in enumerate(lines):
            self.add('<text x="%g" y="%g" text-anchor="middle" font-size="%g" font-weight="%s" fill="%s">%s</text>' % (x + w / 2, ty + i * lh, sz, wt, c, esc(t)))
        if nid in self.steps:
            self.add('<rect class="hl" style="animation-delay:%.2fs" x="%g" y="%g" width="%g" height="%g" rx="9" fill="%s" fill-opacity="0.10" stroke="%s" stroke-width="3"/>' % (
                self.delay(nid), x - 1, y - 1, w + 2, h + 2, ACCENT, ACCENT))
        self.add('</g>')

    def delay(self, name):
        return self.steps.index(name) * self.step

    def edge_svg(self, eid, e):
        pts = self.route(e)
        d = 'M' + ' L'.join('%g,%g' % p for p in pts)
        kind = e.get('kind', 'plain')
        col = KINDS[kind][1] if kind != 'plain' else '#7d8c9b'
        dash = ' stroke-dasharray="5 4"' if e.get('dashed') else ''
        self.add('<g id="%s">' % esc(eid))
        self.add('<path d="%s" fill="none" stroke="%s" stroke-width="1.8"%s stroke-linejoin="round" marker-end="url(#ar-%s)"/>' % (d, col, dash, kind))
        if eid in self.steps:
            self.add('<path class="hl flow" style="animation-delay:%.2fs" d="%s" fill="none" stroke="%s" stroke-width="3.4" stroke-linejoin="round" marker-end="url(#ar-hl)"/>' % (
                self.delay(eid), d, ACCENT))
        label = e.get('label')
        if label:
            # longest segment; a label sits above a horizontal line and beside a vertical one
            segs = list(zip(pts[:-1], pts[1:]))
            a, b = max(segs, key=lambda s: abs(s[0][0] - s[1][0]) + abs(s[0][1] - s[1][1]))
            horizontal = abs(a[1] - b[1]) < 0.5
            lx, ly = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
            if horizontal:
                lx += e.get('dx', 0); ly += e.get('dy', -13)
                anchor, bx = 'middle', None
            else:
                lx += e.get('dx', 9); ly += e.get('dy', 0)
                anchor = 'start'
            lw = text_w(label, 11.5) + 10
            bx = lx - lw / 2 if anchor == 'middle' else lx - 5
            self.add('<rect x="%g" y="%g" width="%g" height="17" rx="4" fill="%s" fill-opacity="0.85"/>' % (bx, ly - 9, lw, BG))
            self.add('<text x="%g" y="%g" text-anchor="%s" font-size="11.5" fill="%s">%s</text>' % (lx, ly + 3.5, anchor, FG, esc(label)))
        self.add('</g>')

    def note_svg(self, t):
        anchor = t.get('anchor', 'start')
        kind = t.get('kind')
        col = KINDS[kind][1] if kind else MUTED
        size = t.get('size', 12)
        for i, line in enumerate(t['text'].split('\n')):
            self.add('<text x="%g" y="%g" text-anchor="%s" font-size="%g" fill="%s"%s>%s</text>' % (
                t['x'], t['y'] + i * (size + 5), anchor, size, col, ' font-weight="bold"' if t.get('bold') else '', esc(line)))

    # ---------- flow diagrams
    def flow(self):
        d = self.d
        self.nodes = {}
        for nid, n in d['nodes'].items():
            self.nodes[nid] = dict(n)
        self.edges = []
        for i, e in enumerate(d.get('edges', [])):
            self.edges.append((e.get('id', 'e%d' % i), e))
        self.steps = list(d.get('steps', []))
        known = set(self.nodes) | {eid for eid, _ in self.edges}
        for s in self.steps:
            if s not in known:
                raise SystemExit('%s: unknown step %r' % (self.id, s))
        for t in d.get('notes', []):
            pass
        for eid, e in self.edges:
            self.edge_svg(eid, e)
        for nid, n in self.nodes.items():
            self.node_svg(nid, n)
        for t in d.get('notes', []):
            self.note_svg(t)

    # ---------- timeline diagrams
    def timeline(self):
        d = self.d
        self.steps = []
        left = d.get('label_width', 120)
        scale = d['scale']                   # pixels per unit
        top = d.get('top', 40)
        lane_h = d.get('lane_h', 64)
        total = d['length'] * scale
        for i, lane in enumerate(d['lanes']):
            y = top + i * (lane_h + 14)
            self.add('<rect x="10" y="%g" width="%g" height="%g" rx="8" fill="#171d25" stroke="#2a3441"/>' % (y, self.w - 20, lane_h))
            self.add('<text x="22" y="%g" font-size="13" font-weight="bold" fill="%s">%s</text>' % (y + lane_h / 2 + 4, FG, esc(lane['name'])))
            for sg in lane['segments']:
                fill, stroke = KINDS[sg.get('kind', 'plain')]
                x = left + sg['t0'] * scale
                w = max((sg['t1'] - sg['t0']) * scale, 4)
                self.add('<rect x="%g" y="%g" width="%g" height="%g" rx="4" fill="%s" stroke="%s" stroke-width="1.5"/>' % (x, y + 12, w, lane_h - 24, fill, stroke))
                lab = sg['label']
                if text_w(lab, 11.5) <= w - 8:
                    self.add('<text x="%g" y="%g" text-anchor="middle" font-size="11.5" fill="%s">%s</text>' % (x + w / 2, y + lane_h / 2 + 4, FG, esc(lab)))
                else:
                    right = x + text_w(lab, 11) > self.w - 10
                    self.add('<text x="%g" y="%g" text-anchor="%s" font-size="11" fill="%s">%s</text>' % (x + w if right else x - 2, y + 9, 'end' if right else 'start', MUTED, esc(lab)))
        h_all = top + len(d['lanes']) * (lane_h + 14) - 14
        # time axis ticks
        for t, lab in d.get('ticks', []):
            x = left + t * scale
            self.add('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="#2a3441" stroke-dasharray="2 4"/>' % (x, top - 8, x, h_all + 6))
            end = x + text_w(lab, 11) / 2 > self.w - 8
            self.add('<text x="%g" y="%g" text-anchor="%s" font-size="11" fill="%s">%s</text>' % (self.w - 12 if end else x, h_all + 22, 'end' if end else 'middle', MUTED, esc(lab)))
        # playhead
        dur = d.get('play', 9)
        self.add('<g class="play" style="animation-duration:%gs; --dx:%gpx"><line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" stroke-width="2.5"/></g>' % (
            dur, total, left, top - 10, left, h_all + 8, ACCENT))
        for t in d.get('notes', []):
            self.note_svg(t)
        self.timeline_dur = dur

    # ---------- shared: numbered captions that follow the callouts
    def caption(self, i, title, text, cap_y, delay, bg_y, bg_h, attr=None):
        self.add('<g %s>' % (attr or 'class="co cap" style="animation-delay:%.2fs"' % delay))
        self.add('<rect x="0" y="%g" width="%g" height="%g" fill="%s"/>' % (bg_y, self.w, bg_h, BG))
        self.add('<circle cx="24" cy="%g" r="13" fill="%s"/><text x="24" y="%g" text-anchor="middle" font-size="14" font-weight="bold" fill="#12171d">%d</text>' % (cap_y, ACCENT, cap_y + 5, i + 1))
        self.add('<text x="48" y="%g" font-size="15" font-weight="bold" fill="%s">%s</text>' % (cap_y - 2, FG, esc(title)))
        self.add('<text x="48" y="%g" font-size="13" fill="%s">%s</text>' % (cap_y + 18, MUTED, esc(text)))
        self.add('</g>')

    # ---------- recorded console output with callouts
    def terminal(self):
        d = self.d
        lines = d['lines']
        cw, lh, top = 7.8, 20, 58
        limit = int((self.w - 48) / cw)
        for i, ln in enumerate(lines):
            if len(ln) > limit:
                raise SystemExit('%s: terminal line %d is %d characters, the limit is %d: %r' % (self.id, i, len(ln), limit, ln[:40]))
        ph = top + len(lines) * lh + 16
        self.steps = ['c%d' % i for i in range(len(d['callouts']))]
        self.add('<rect x="0" y="0" width="%g" height="%g" fill="#0d1117"/>' % (self.w, ph))
        self.add('<rect x="0" y="0" width="%g" height="34" fill="#1b222c"/>' % self.w)
        for k, c in enumerate(('#ef6f6c', '#e6c04f', '#5fbf84')):
            self.add('<circle cx="%d" cy="17" r="6" fill="%s"/>' % (20 + 18 * k, c))
        self.add('<text x="%g" y="22" text-anchor="middle" font-size="12" fill="%s">%s</text>' % (self.w / 2, MUTED, esc(d.get('title', 'terminal'))))
        mono = 'font-family="IBM Plex Mono, ui-monospace, Menlo, Consolas, monospace" font-size="13"'
        for i, ln in enumerate(lines):
            y = top + i * lh
            if ln.startswith('$ ') or ln.startswith('# '):
                self.add('<text x="24" y="%g" %s xml:space="preserve"><tspan fill="#5fbf84">%s</tspan><tspan fill="%s">%s</tspan></text>' % (y, mono, esc(ln[:2]), FG, esc(ln[2:])))
            else:
                self.add('<text x="24" y="%g" %s fill="#b9c4cf" xml:space="preserve">%s</text>' % (y, mono, esc(ln)))
        cap_y = ph + 26
        self.add('<rect x="0" y="%g" width="%g" height="%g" fill="%s"/>' % (ph, self.w, self.h - ph, BG))
        if self.h < ph + 70:
            raise SystemExit('%s: height %g is too small, need at least %g' % (self.id, self.h, ph + 70))
        for i, c in enumerate(d['callouts']):
            a, b = c['lines'][0], c['lines'][-1]
            y0 = top + a * lh - 15
            h = (b - a + 1) * lh + 3
            dl = self.delay('c%d' % i)
            self.add('<g class="co" style="animation-delay:%.2fs"><rect x="8" y="%g" width="%g" height="%g" rx="4" fill="%s" fill-opacity="0.13" stroke="%s" stroke-width="2.5"/>' % (dl, y0, self.w - 16, h, ACCENT, ACCENT))
            self.add('<circle cx="8" cy="%g" r="11" fill="%s"/><text x="8" y="%g" text-anchor="middle" font-size="12" font-weight="bold" fill="#12171d">%d</text></g>' % (y0 + 2, ACCENT, y0 + 6, i + 1))
            self.caption(i, c['title'], c['text'], cap_y, dl, ph, self.h - ph)

    # ---------- waveforms from a recorded simulation, with callouts
    def wave(self):
        d = self.d
        left, right, top, rh = 140, 20, 44, 40
        length = d['length']
        sc = (self.w - left - right) / length
        sigs = d['signals']
        ph = top + len(sigs) * rh + 12
        self.steps = ['c%d' % i for i in range(len(d.get('callouts', [])))]
        if self.h < ph + 74:
            raise SystemExit('%s: height %g is too small, need at least %g' % (self.id, self.h, ph + 74))
        self.add('<rect x="0" y="0" width="%g" height="%g" fill="#0d1117"/>' % (self.w, ph))
        for t, lab in d.get('ticks', []):
            x = left + t * sc
            self.add('<line x1="%g" y1="30" x2="%g" y2="%g" stroke="#27313d" stroke-dasharray="2 4"/><text x="%g" y="22" text-anchor="middle" font-size="11" fill="%s">%s</text>' % (x, x, ph - 6, x, MUTED, esc(lab)))
        for r, sg in enumerate(sigs):
            y = top + r * rh
            col = KINDS[sg.get('kind', 'plain')][1] if sg.get('kind') else '#8fb0d8'
            self.add('<text x="12" y="%g" font-size="12" fill="%s" font-family="IBM Plex Mono, ui-monospace, monospace">%s</text>' % (y + 22, FG, esc(sg['name'])))
            pts = sg['values']
            if sg.get('bus'):
                for k, (t, v) in enumerate(pts):
                    t1 = pts[k + 1][0] if k + 1 < len(pts) else length
                    x0, x1 = left + t * sc, left + t1 * sc
                    w = max(x1 - x0, 1)
                    self.add('<rect x="%g" y="%g" width="%g" height="22" rx="4" fill="#1b2a3a" stroke="%s" stroke-width="1.2"/>' % (x0 + 0.5, y + 9, max(w - 1, 1), col))
                    if text_w(str(v), 11) <= w - 6:
                        self.add('<text x="%g" y="%g" text-anchor="middle" font-size="11" fill="%s" font-family="IBM Plex Mono, ui-monospace, monospace">%s</text>' % (x0 + w / 2, y + 24, FG, esc(v)))
            else:
                path = []
                prev = None
                for t, v in pts:
                    x = left + t * sc
                    yy = y + (10 if v else 30)
                    path.append(('M' if prev is None else 'L') + '%g,%g' % (x, yy if prev is None else prev))
                    path.append('L%g,%g' % (x, yy))
                    prev = yy
                path.append('L%g,%g' % (left + length * sc, prev))
                self.add('<path d="%s" fill="none" stroke="%s" stroke-width="2" stroke-linejoin="round"/>' % (' '.join(path), col))
        cap_y = ph + 28
        self.add('<rect x="0" y="%g" width="%g" height="%g" fill="%s"/>' % (ph, self.w, self.h - ph, BG))
        for i, c in enumerate(d.get('callouts', [])):
            x0, x1 = left + c['t0'] * sc, left + c['t1'] * sc
            dl = self.delay('c%d' % i)
            self.add('<g class="co" style="animation-delay:%.2fs"><rect x="%g" y="30" width="%g" height="%g" rx="4" fill="%s" fill-opacity="0.13" stroke="%s" stroke-width="2.5"/>' % (dl, x0, max(x1 - x0, 4), ph - 36, ACCENT, ACCENT))
            self.add('<circle cx="%g" cy="30" r="11" fill="%s"/><text x="%g" y="34" text-anchor="middle" font-size="12" font-weight="bold" fill="#12171d">%d</text></g>' % (x0, ACCENT, x0, i + 1))
            self.caption(i, c['title'], c['text'], cap_y, dl, ph, self.h - ph)

    # ---------- annotated screenshots
    def annotated(self):
        """A screenshot with numbered callouts that light up one after the other, each with its caption."""
        import base64
        from PIL import Image
        d = self.d
        path = os.path.join(ROOT, d['image'])
        im = Image.open(path)
        w = self.w
        sc = w / im.size[0]
        ih = im.size[1] * sc
        data = base64.b64encode(open(path, 'rb').read()).decode()
        self.add('<image href="data:image/png;base64,%s" x="0" y="0" width="%g" height="%g"/>' % (data, w, ih))
        self.steps = ['c%d' % i for i in range(len(d['callouts']))]
        cap_y = ih + 26
        self.add('<rect x="0" y="%g" width="%g" height="%g" fill="%s"/>' % (ih, w, self.h - ih, BG))
        for i, c in enumerate(d['callouts']):
            x, y, bw, bh = [v * sc for v in c['box']]
            dl = self.delay('c%d' % i)
            self.add('<g class="co" style="animation-delay:%.2fs">' % dl)
            self.add('<rect x="%g" y="%g" width="%g" height="%g" rx="4" fill="%s" fill-opacity="0.12" stroke="%s" stroke-width="3"/>' % (x - 3, y - 3, bw + 6, bh + 6, ACCENT, ACCENT))
            self.add('<circle cx="%g" cy="%g" r="13" fill="%s"/><text x="%g" y="%g" text-anchor="middle" font-size="14" font-weight="bold" fill="#12171d">%d</text>' % (x - 3, y - 3, ACCENT, x - 3, y + 2, i + 1))
            self.add('</g>')
            self.add('<g class="co cap" style="animation-delay:%.2fs">' % dl)
            self.add('<rect x="0" y="%g" width="%g" height="%g" fill="%s"/>' % (ih + 1, w, self.h - ih - 1, BG))
            self.add('<circle cx="24" cy="%g" r="13" fill="%s"/><text x="24" y="%g" text-anchor="middle" font-size="14" font-weight="bold" fill="#12171d">%d</text>' % (cap_y, ACCENT, cap_y + 5, i + 1))
            self.add('<text x="48" y="%g" font-size="15" font-weight="bold" fill="%s">%s</text>' % (cap_y - 2, FG, esc(c['title'])))
            self.add('<text x="48" y="%g" font-size="13" fill="%s">%s</text>' % (cap_y + 18, MUTED, esc(c['text'])))
            self.add('</g>')

    # ---------- a recorded terminal session, played back at its recorded times
    def replay(self):
        import json, bisect
        d = self.d
        MAXEV = 600
        rec = d.get('recording')
        if not rec:
            raise SystemExit('%s: replay needs recording: <file.jsonl>, relative to docs/diagrams' % self.id)
        rec_path = os.path.join(DIR, rec)
        if not os.path.isfile(rec_path):
            raise SystemExit('%s: recording %s not found (path is relative to docs/diagrams)' % (self.id, rec))
        panes = d.get('panes') or []
        if len(panes) not in (2, 3):
            raise SystemExit('%s: replay needs 2 or 3 panes, found %d' % (self.id, len(panes)))
        gap = float(d.get('max_gap', 1.5))
        hold = float(d.get('hold', 3.0))
        lead = 0.5                      # seconds of empty panes before the first line
        cw, lh, mx = 7.8, 18, 10
        limit = int((self.w - 2 * mx - 28) / cw)

        # ---- read and check the recording
        ev = []
        for n, raw in enumerate(open(rec_path, encoding='utf-8'), 1):
            if not raw.strip():
                continue
            try:
                o = json.loads(raw)
                t, pn, tx = float(o['t']), int(o['pane']), str(o['text'])
            except (ValueError, KeyError, TypeError) as e:
                raise SystemExit('%s: %s line %d is not {"t", "pane", "text"}: %s' % (self.id, rec, n, e))
            if t < 0 or (ev and t < ev[-1][0]):
                raise SystemExit('%s: %s line %d: t=%g is negative or earlier than the line before (the file must be sorted by t)' % (self.id, rec, n, t))
            if not 0 <= pn < len(panes):
                raise SystemExit('%s: %s line %d: pane %d does not exist (the figure has %d panes)' % (self.id, rec, n, pn, len(panes)))
            tx = tx.rstrip('\r\n')
            if re.search(r'[\x00-\x1f\x7f]', tx):
                raise SystemExit('%s: %s line %d has control characters (strip colour codes and tabs in the capture script)' % (self.id, rec, n))
            if len(tx) > limit:
                if not d.get('clip'):
                    raise SystemExit('%s: %s line %d is %d characters, the limit is %d (clip: true cuts it): %r' % (self.id, rec, n, len(tx), limit, tx[:40]))
                tx = tx[:limit - 1] + '…'
            ev.append((t, pn, tx))
        if not ev:
            raise SystemExit('%s: %s has no lines' % (self.id, rec))
        if len(ev) > MAXEV:
            raise SystemExit('%s: %d lines, the limit is %d (every line carries its own animation)' % (self.id, len(ev), MAXEV))
        ts = [e[0] for e in ev]

        # ---- time compression: playback time of every line
        tc = [lead]
        for i in range(1, len(ev)):
            tc.append(tc[-1] + min(ts[i] - ts[i - 1], gap))
        D = tc[-1] + hold                       # length of one playback cycle
        pct = lambda x: min(100.0, max(0.0, x / D * 100))

        def tcmap(t):                           # playback time of a recorded time
            if t < ts[0]:
                return max(0.0, lead - (ts[0] - t))
            k = bisect.bisect_right(ts, t) - 1
            return tc[k] + min(t - ts[k], gap)

        def real(x):                            # recorded time shown at playback time x
            if x < lead:
                return max(0.0, ts[0] - (lead - x))
            if x >= tc[-1]:
                return ts[-1]
            k = bisect.bisect_right(tc, x) - 1
            span = tc[k + 1] - tc[k]
            return ts[k] if span <= 0 else ts[k] + (x - tc[k]) / span * (ts[k + 1] - ts[k])
        mmss = lambda t: '%02d:%02d' % (int(t) // 60, int(t) % 60)

        # ---- callouts: playback windows, each lasts until the next one
        callouts = d.get('callouts') or []
        win = []
        for i, c in enumerate(callouts):
            if not (0 <= c['t'] <= ts[-1]):
                raise SystemExit('%s: callout %d at t=%g is outside the recording (0 to %g)' % (self.id, i + 1, c['t'], ts[-1]))
            if i and c['t'] < callouts[i - 1]['t']:
                raise SystemExit('%s: callouts must be in order of t (callout %d)' % (self.id, i + 1))
            for k in c.get('panes') or []:
                if not 0 <= k < len(panes):
                    raise SystemExit('%s: callout %d names pane %d, which does not exist' % (self.id, i + 1, k))
        for i, c in enumerate(callouts):
            a = tcmap(c['t'])
            b = tcmap(callouts[i + 1]['t']) if i + 1 < len(callouts) else D
            win.append((a, max(b, a + 0.3)))

        kfs, names, E = [], {}, 0.01

        def window(a, b):                       # name of an opacity animation that is 1 between a and b
            key = (round(a, 3), round(b, 3))
            if key not in names:
                names[key] = '%s_w%d' % (self.id, len(names))
                pa, pb = pct(a), pct(b)
                body = '0%%,%.3f%%{opacity:0}%.3f%%,%.3f%%{opacity:1}' % (pa, min(pa + E, pb), pb)
                if pb < 100 - E:
                    body += '%.3f%%,100%%{opacity:0}' % (pb + E)
                kfs.append('@keyframes %s{%s}' % (names[key], body))
            return names[key]

        # ---- layout
        top = 30
        rows = [int(p.get('rows', d.get('rows', 8))) for p in panes]
        geo, y = [], top
        for r in rows:
            geo.append((y, 26 + r * lh + 12))
            y += 26 + r * lh + 12 + 8
        ph = y
        need = ph + 74
        if self.h and self.h < need:
            raise SystemExit('%s: height %g is too small, need at least %g' % (self.id, self.h, need))
        if not self.h:
            self.h = need
        cap_y = ph + 28
        self.steps = []
        mono = 'font-family="IBM Plex Mono, ui-monospace, Menlo, Consolas, monospace" font-size="13"'

        # ---- top strip: compression note and the recorded-time clock
        self.add('<text x="%g" y="20" font-size="12" fill="%s">gaps over %g s are shortened</text>' % (mx + 4, MUTED, gap))
        xr = self.w - mx - 4
        self.add('<text x="%g" y="20" text-anchor="end" font-size="12" fill="%s">of %s</text>' % (xr, MUTED, mmss(ts[-1])))
        xc = xr - 7.0 * len('of %s' % mmss(ts[-1])) - 6
        self.add('<text x="%g" y="20" text-anchor="end" font-size="12" fill="%s">recorded</text>' % (xc - 42, MUTED))
        runs, x = [], 0.0
        while x <= D:
            s_ = mmss(real(x))
            if runs and runs[-1][0] == s_:
                runs[-1][2] = x + 0.25
            else:
                runs.append([s_, x, x + 0.25])
            x += 0.25
        for s_, a, b in runs:
            self.add('<text class="rw" style="animation-name:%s" x="%g" y="20" font-size="12" font-weight="bold" fill="%s" text-anchor="end">%s</text>' % (
                window(a, min(b, D)), xc, FG, s_))
        self.add('<text class="rs" x="%g" y="20" font-size="12" font-weight="bold" fill="%s" text-anchor="end">%s</text>' % (xc, FG, mmss(ts[-1])))

        # ---- panes
        per = [[i for i, e in enumerate(ev) if e[1] == k] for k in range(len(panes))]
        scroll = []
        nl = 0
        for k, p in enumerate(panes):
            y0, hh = geo[k]
            N = rows[k]
            title = p.get('title', 'pane %d' % (k + 1))
            if text_w(title, 12) > self.w - 2 * mx - 70 - 90:
                raise SystemExit('%s: pane title too long: %r' % (self.id, title))
            clip = '%s-clip%d' % (self.id, k)
            self.defs_extra += '<clipPath id="%s"><rect x="%g" y="%g" width="%g" height="%g"/></clipPath>' % (clip, mx, y0 + 31, self.w - 2 * mx, hh - 31)
            self.add('<rect x="%g" y="%g" width="%g" height="%g" rx="8" fill="#0d1117"/>' % (mx, y0, self.w - 2 * mx, hh))
            self.add('<path d="M%g %g a8 8 0 0 1 8 -8 h%g a8 8 0 0 1 8 8 v18 h-%g z" fill="#1b222c"/>' % (mx, y0 + 8, self.w - 2 * mx - 16, self.w - 2 * mx))
            for j, c in enumerate(('#ef6f6c', '#e6c04f', '#5fbf84')):
                self.add('<circle cx="%d" cy="%g" r="5" fill="%s"/>' % (mx + 16 + 16 * j, y0 + 13, c))
            self.add('<text x="%g" y="%g" font-size="12" fill="%s">%s</text>' % (mx + 70, y0 + 17, MUTED, esc(title)))
            idxs = per[k]
            stops = {}
            for r_, i in enumerate(idxs):
                if r_ >= N:
                    stops[round(pct(tc[i]), 4)] = -(r_ - N + 1) * lh
            final = -(len(idxs) - N) * lh if len(idxs) > N else 0
            sk = '%s_s%d' % (self.id, k)
            if stops:
                scroll.append('@keyframes %s{0%%{transform:translateY(0)}%s}' % (sk, ''.join(
                    '%.4f%%{transform:translateY(%dpx)}' % (q, v) for q, v in sorted(stops.items()))))
            self.add('<g clip-path="url(#%s)"><g class="rg"%s transform="translate(0 %d)">' % (
                clip, ' style="animation-name:%s"' % sk if stops else '', final))
            for r_, i in enumerate(idxs):
                tx = ev[i][2]
                yy = y0 + 26 + 18 + r_ * lh - 2
                kn = '%s_l%d' % (self.id, nl)
                nl += 1
                left = 100 - pct(tc[i])
                kfs.append('@keyframes %s{0%%,%.3f%%{opacity:1}%.3f%%,100%%{opacity:0}}' % (kn, left, min(100, left + E)))
                if tx.startswith('$ ') or tx.startswith('# '):
                    inner, fill = '<tspan fill="#5fbf84">%s</tspan><tspan fill="%s">%s</tspan>' % (esc(tx[:2]), FG, esc(tx[2:])), ''
                else:
                    inner, fill = esc(tx), ' fill="#b9c4cf"'
                self.add('<text class="rl" style="animation-name:%s;animation-delay:%.3fs" x="%g" y="%g" %s%s xml:space="preserve">%s</text>' % (
                    kn, tc[i], mx + 14, yy, mono, fill, inner))
            for ci, c in enumerate(callouts):          # the highlighted line moves with the scrolling text
                if k in (c.get('panes') or []):
                    seen = [i for i in idxs if ts[i] <= c['t']]
                    if seen:
                        r_ = len(seen) - 1
                        self.add('<rect class="rw" style="animation-name:%s" x="%g" y="%g" width="%g" height="%g" rx="3" fill="%s" fill-opacity="0.15" stroke="%s" stroke-width="2"/>' % (
                            window(*win[ci]), mx + 6, y0 + 26 + 4 + r_ * lh, self.w - 2 * mx - 12, lh, ACCENT, ACCENT))
            self.add('</g></g>')
            for ci, c in enumerate(callouts):          # pane outline and numbered badge in the title bar
                if k in (c.get('panes') or []):
                    self.add('<g class="rw" style="animation-name:%s"><rect x="%g" y="%g" width="%g" height="%g" rx="8" fill="none" stroke="%s" stroke-width="2.5"/>'
                             '<circle cx="%g" cy="%g" r="10" fill="%s"/><text x="%g" y="%g" text-anchor="middle" font-size="12" font-weight="bold" fill="#12171d">%d</text></g>' % (
                                 window(*win[ci]), mx, y0, self.w - 2 * mx, hh, ACCENT, self.w - mx - 20, y0 + 13, ACCENT, self.w - mx - 20, y0 + 17, ci + 1))
        self.add('<rect x="0" y="%g" width="%g" height="%g" fill="%s"/>' % (ph, self.w, self.h - ph, BG))
        for ci, c in enumerate(callouts):
            self.caption(ci, c['title'], c['text'], cap_y, 0, ph, self.h - ph,
                         attr='class="rw" style="animation-name:%s"' % window(*win[ci]))
        self.replay_dur = D
        self.css_extra = (
            '.rw,.rl{opacity:0}.rw,.rl,.rg{animation-duration:%.3fs;animation-timing-function:linear;animation-iteration-count:infinite}'
            '.rg{animation-timing-function:step-end}.rs{display:none}'
            '@media (prefers-reduced-motion:reduce){.rw,.rl,.rg{animation:none}.rl{opacity:1}.rw{display:none}.rs{display:inline}}\n%s\n%s' % (
                D, '\n'.join(kfs), '\n'.join(scroll)))

    # ---------- document
    def render(self):
        d = self.d
        if d.get('type') == 'timeline':
            self.timeline()
            cycle = self.timeline_dur
        elif d.get('type') == 'replay':
            self.replay()
            cycle = self.replay_dur
        elif d.get('type') in ('annotated', 'terminal', 'wave'):
            getattr(self, d['type'])()
            cycle = max(len(self.steps) * self.step + HOLD, 1)
        else:
            self.flow()
            cycle = max(len(self.steps) * self.step + HOLD, 1)
        body = '\n'.join(self.out)
        on = (len(self.steps) and self.step / cycle * 100) or 0
        css = """
text{font-family:%s}
.hl,.co{opacity:0;animation:lit %.2fs linear infinite;animation-fill-mode:forwards}
.flow{stroke-dasharray:9 6;animation:lit %.2fs linear infinite,flow .7s linear infinite;animation-fill-mode:forwards}
@keyframes lit{0%%{opacity:0}2%%{opacity:1}%.2f%%{opacity:1}%.2f%%{opacity:0}100%%{opacity:0}}
@keyframes flow{to{stroke-dashoffset:-15}}
.play{animation-name:slide;animation-timing-function:linear;animation-iteration-count:infinite}
@keyframes slide{from{transform:translateX(0)}to{transform:translateX(var(--dx))}}
@media (prefers-reduced-motion:reduce){.hl,.play{animation:none}.play{display:none}.co{animation:none;opacity:1}.cap{display:none}}
""" % (FONT, cycle, cycle, on, on + 3)
        title = d.get('alt', self.id)
        marks = ''.join(
            '<marker id="ar-%s" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="%s"/></marker>' % (k, c)
            for k, c in [('plain', '#7d8c9b')] + [(k, v[1]) for k, v in KINDS.items() if k != 'plain'] + [('hl', ACCENT)])
        head = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %g %g" width="%g" height="%g" role="img" aria-labelledby="t d">' % (self.w, self.h, self.w, self.h)
        css += self.css_extra
        marks += self.defs_extra
        return '%s\n<title id="t">%s</title><desc id="d">%s</desc>\n<style>%s</style>\n<defs>%s</defs>\n<rect width="%g" height="%g" rx="12" fill="%s"/>\n%s\n</svg>\n' % (
            head, esc(d.get('title', title)), esc(title), css, marks, self.w, self.h, BG, body)


def main():
    specs = []
    for name in sorted(os.listdir(DIR)):
        if name.endswith('.yaml'):
            specs += yaml.safe_load(open(os.path.join(DIR, name))) or []
    seen = set()
    for d in specs:
        if d['id'] in seen:
            raise SystemExit('duplicate diagram id %s' % d['id'])
        seen.add(d['id'])
    stale = []
    for d in specs:
        svg = Diagram(d).render()
        path = os.path.join(DIR, d['id'] + '.svg')
        cur = open(path).read() if os.path.isfile(path) else None
        if cur != svg:
            stale.append(d['id'])
            if '--check' not in sys.argv:
                open(path, 'w').write(svg)
    ids = {d['id'] for d in specs}
    extra = [f for f in os.listdir(DIR) if f.endswith('.svg') and f[:-4] not in ids]
    if '--check' in sys.argv:
        if stale or extra:
            print('diagrams out of date: %s %s' % (stale, extra))
            return 1
        print('diagrams: %d current' % len(specs))
        return 0
    print('%d diagrams, %d written' % (len(specs), len(stale)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
