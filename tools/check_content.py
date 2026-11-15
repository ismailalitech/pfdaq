#!/usr/bin/env python3
"""Consistency check for the pfdaq documentation. Exits non-zero on any error.

Checks that
  - the architecture data is well formed (unique ids, parents, edge endpoints, path steps);
  - every page has the required front matter and legal values;
  - every reference a page makes resolves: prerequisite and related modules, architecture nodes,
    source files and the symbols named in them, evidence entries, relative links;
  - every evidence manifest entry points at a file that exists;
  - generated files are up to date (run tools/gen_docs.py to refresh them).
"""
import os
import re
import subprocess
import sys

import content as C

errors = []


def err(where, msg):
    errors.append('%s: %s' % (where, msg))


def check_sources(where, sources):
    for s in C.norm_sources(sources):
        path = os.path.join(C.ROOT, s['path'])
        if not os.path.isfile(path):
            err(where, 'source file not found: %s' % s['path'])
            continue
        if s.get('symbols'):
            with open(path, errors='replace') as f:
                text = f.read()
            for sym in s['symbols']:
                if not re.search(r'\b%s\b' % re.escape(sym), text):
                    err(where, 'symbol %s not found in %s' % (sym, s['path']))


def main():
    arch = C.load_architecture()
    nodes = {}
    for n in arch['nodes']:
        if n['id'] in nodes:
            err('architecture', 'duplicate node id %s' % n['id'])
        nodes[n['id']] = n
    for n in arch['nodes']:
        if n.get('parent') and n['parent'] not in nodes:
            err('architecture', 'node %s: unknown parent %s' % (n['id'], n['parent']))
        for k in ('title', 'kind', 'summary'):
            if not n.get(k):
                err('architecture', 'node %s: missing %s' % (n['id'], k))
        check_sources('architecture node %s' % n['id'], n.get('sources'))
    edges = {}
    for e in arch['edges']:
        if e['id'] in edges:
            err('architecture', 'duplicate edge id %s' % e['id'])
        edges[e['id']] = e
        for end in ('from', 'to'):
            if e[end] not in nodes:
                err('architecture', 'edge %s: unknown node %s' % (e['id'], e[end]))
    for p in arch.get('paths', []):
        for step in p['steps']:
            if step['edge'] not in edges:
                err('architecture', 'path %s: unknown edge %s' % (p['id'], step['edge']))

    evidence = C.load_evidence()
    for eid, e in evidence.items():
        if not os.path.isfile(os.path.join(C.ROOT, e['path'])):
            err('evidence %s' % eid, 'file not found: %s' % e['path'])
        for k in ('title', 'class', 'fabric', 'date', 'command'):
            if not e.get(k):
                err('evidence %s' % eid, 'missing %s' % k)
        if e.get('class') not in ('board', 'sim', 'build', 'host'):
            err('evidence %s' % eid, 'class must be board, sim, build or host')

    pages = C.load_pages()
    planned = C.load_catalog()
    ids = {}
    for p in pages:
        if p.get('_no_front_matter'):
            err(p['file'], 'front matter missing or not valid YAML (quote any value that contains a colon): %s' % p.get('_error', ''))
            continue
        for k in C.REQUIRED:
            if p.get(k) in (None, ''):
                err(p['file'], 'missing front matter field: %s' % k)
        if p.get('id') in ids:
            err(p['file'], 'duplicate id %s (also %s)' % (p['id'], ids[p['id']]))
        ids[p.get('id')] = p['file']
    for m in planned:
        if m['id'] in ids:
            err('catalog.yaml', '%s is written (%s): remove it from the catalog' % (m['id'], ids[m['id']]))
        ids[m['id']] = 'catalog.yaml'
        for k in ('id', 'title', 'track', 'level', 'summary', 'repro'):
            if m.get(k) in (None, ''):
                err('catalog.yaml', '%s: missing %s' % (m.get('id'), k))

    for p in pages + planned:
        where = p.get('file', 'catalog.yaml %s' % p.get('id'))
        if p.get('_no_front_matter'):
            continue
        if not C.track_of(p):
            err(where, 'unknown track: %s' % p.get('track'))
        if p.get('repro') not in C.REPRO:
            err(where, 'unknown repro: %s' % p.get('repro'))
        if p.get('status') not in C.STATUS:
            err(where, 'unknown status: %s' % p.get('status'))
        if p.get('level') not in (1, 2, 3, 4, 5):
            err(where, 'level must be 1 to 5')
        for k in ('needs', 'related'):
            for ref in p.get(k) or []:
                if ref not in ids:
                    err(where, '%s: unknown module %s' % (k, ref))
        for k in ('hardware', 'software'):
            for ref in p.get(k) or []:
                if ref not in nodes:
                    err(where, '%s: unknown architecture node %s' % (k, ref))
        check_sources(where, p.get('sources'))
        for ref in p.get('evidence') or []:
            if ref not in evidence:
                err(where, 'unknown evidence id %s' % ref)
        if p.get('status') == 'validated' and 'file' in p and not p.get('evidence') and p.get('repro') != 'none':
            err(where, 'status validated needs at least one evidence entry')

    # relative links in every Markdown file under docs/, experiments/ and the top level
    md = subprocess.run(['git', '-C', C.ROOT, 'ls-files', '-co', '--exclude-standard', '*.md'],
                        capture_output=True, text=True).stdout.split()
    for f in md:
        if f.startswith('logs/') or f == 'PROJECT_STATE.md':
            continue
        with open(os.path.join(C.ROOT, f), errors='replace') as fh:
            text = fh.read()
        text = re.sub(r'```.*?```', '', text, flags=re.S)
        for target in re.findall(r'\]\(([^)\s]+)\)', text):
            if re.match(r'[a-z]+:', target) or target.startswith('#'):
                continue
            path = os.path.normpath(os.path.join(os.path.dirname(f), target.split('#')[0]))
            if not os.path.exists(os.path.join(C.ROOT, path)):
                err(f, 'broken link: %s' % target)

    # generated files must be current
    r = subprocess.run([sys.executable, os.path.join(C.ROOT, 'tools', 'gen_docs.py'), '--check'],
                       capture_output=True, text=True)
    if r.returncode != 0:
        err('generated files', (r.stdout + r.stderr).strip() or 'out of date')

    r = subprocess.run([sys.executable, os.path.join(C.ROOT, 'tools', 'gen_explorer.py'), '--check'],
                       capture_output=True, text=True)
    if r.returncode != 0:
        err('explorer', (r.stdout + r.stderr).strip() or 'out of date')

    for e in errors:
        print('ERROR ' + e)
    written = [p for p in pages if not p.get('_no_front_matter')]
    print('%d architecture nodes, %d edges, %d written pages, %d planned modules, %d evidence entries: %s' % (
        len(nodes), len(edges), len(written), len(planned), len(evidence),
        'OK' if not errors else '%d error(s)' % len(errors)))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
