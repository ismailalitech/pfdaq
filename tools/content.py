"""Shared loader for the pfdaq documentation: architecture data, module pages, catalog, evidence.

Used by check_content.py (consistency) and gen_docs.py (generated indexes and navigation).
"""
import os
import re
import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, 'docs')
PAGE_DIRS = ['concepts', 'modules', 'case-studies']

TRACKS = [
    ('fundamentals', 'F', 'PolarFire SoC fundamentals'),
    ('boot', 'B', 'Boot and Linux'),
    ('multicore', 'M', 'Multicore and RTOS'),
    ('fpga', 'H', 'FPGA design'),
    ('integration', 'I', 'Linux to FPGA integration'),
    ('performance', 'P', 'Performance and reliability'),
    ('case-study', 'K', 'Case studies'),
    ('system', 'X', 'The complete system'),
    ('security', 'S', 'Security and secure boot'),
    ('concept', 'C', 'Concepts'),
]
REPRO = {
    'sim': 'Simulation only: Icarus Verilog and Python, no board',
    'base': 'Runs on the shared baseline: fabric v0.5.1 and the pfdaq image',
    'dedicated-fabric': 'Needs its own fabric build',
    'full-system': 'Needs the complete integrated system',
    'doc': 'Explanation with recorded evidence; nothing to run',
    'none': 'No board or tools needed',
}
STATUS = ['validated', 'validated-old', 'simulated', 'experimental', 'planned']
REQUIRED = ['id', 'title', 'track', 'level', 'summary', 'repro', 'status']


def rel(path):
    return os.path.relpath(path, ROOT)


def load_architecture():
    with open(os.path.join(DOCS, 'system', 'architecture.yaml')) as f:
        return yaml.safe_load(f)


def load_evidence():
    """Returns {evidence id: entry} over every evidence/<version>/manifest.yaml."""
    out = {}
    base = os.path.join(ROOT, 'evidence')
    if not os.path.isdir(base):
        return out
    for ver in sorted(os.listdir(base)):
        man = os.path.join(base, ver, 'manifest.yaml')
        if not os.path.isfile(man):
            continue
        with open(man) as f:
            data = yaml.safe_load(f) or {}
        for e in data.get('results', []):
            e['version'] = ver
            e['path'] = os.path.join('evidence', ver, e['file'])
            out[e['id']] = e
    return out


def split_front_matter(text):
    if not text.startswith('---\n'):
        return None, text
    end = text.find('\n---\n', 4)
    if end < 0:
        return None, text
    return yaml.safe_load(text[4:end]), text[end + 5:]


def load_pages():
    """Returns a list of written pages: front matter plus 'file' (repo-relative) and 'body'."""
    pages = []
    for d in PAGE_DIRS:
        full = os.path.join(DOCS, d)
        if not os.path.isdir(full):
            continue
        for name in sorted(os.listdir(full)):
            if not name.endswith('.md') or name == 'README.md':
                continue
            path = os.path.join(full, name)
            with open(path) as f:
                try:
                    meta, body = split_front_matter(f.read())
                except yaml.YAMLError as e:
                    meta, body = {'_error': str(e).replace('\n', ' ')}, ''
            if meta is None:
                meta = {'_no_front_matter': True}
            if '_error' in meta:
                meta['_no_front_matter'] = True
            meta['file'] = rel(path)
            meta['body'] = body
            pages.append(meta)
    return pages


def load_catalog():
    """Planned modules that have no page yet."""
    path = os.path.join(DOCS, 'modules', 'catalog.yaml')
    if not os.path.isfile(path):
        return []
    with open(path) as f:
        items = yaml.safe_load(f) or []
    for m in items:
        m.setdefault('status', 'planned')
    return items


def all_modules():
    """Written pages first, then planned catalog entries, as {id: meta}."""
    out = {}
    for p in load_pages():
        if 'id' in p:
            out[p['id']] = p
    for m in load_catalog():
        out.setdefault(m['id'], m)
    return out


def track_of(meta):
    for key, letter, title in TRACKS:
        if meta.get('track') == key:
            return key, letter, title
    return None


def module_sort_key(meta):
    m = re.match(r'([A-Z]+)(\d+)', meta['id'])
    order = [t[1] for t in TRACKS]
    letter = m.group(1) if m else 'Z'
    return (order.index(letter) if letter in order else 99, int(m.group(2)) if m else 0)


def norm_sources(sources):
    """Sources are written either as {path, symbols} or as 'path' / 'path:sym1,sym2'."""
    out = []
    for s in sources or []:
        if isinstance(s, str):
            path, _, syms = s.partition(':')
            s = {'path': path, 'symbols': [x for x in syms.split(',') if x]}
        out.append(s)
    return out


def fabric_label(e):
    """', fabric v0.5.1' for a result tied to a fabric version, '' for host-only results."""
    return '' if e['fabric'] == 'n/a' else ', fabric v%s' % e['fabric']
