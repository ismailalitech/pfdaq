#!/usr/bin/env python3
"""Remove written modules from docs/modules/catalog.yaml: tools/catalog_remove.py H02 K03 ..."""
import os, sys, yaml
p = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'docs', 'modules', 'catalog.yaml')
text = open(p).read()
head = ''.join(l for l in text.splitlines(True) if l.startswith('#'))
items = [m for m in yaml.safe_load(text) if m['id'] not in sys.argv[1:]]
open(p, 'w').write(head + yaml.safe_dump(items, sort_keys=False, allow_unicode=True, width=200))
print('%d planned modules left' % len(items))
