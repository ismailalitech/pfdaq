#!/usr/bin/env python3
"""Generate the architecture explorer, docs/system/explorer.html, from docs/system/architecture.yaml,
docs/system/layout.yaml and the module metadata.

The diagram is drawn here as static SVG, so the page is readable without JavaScript. A small inline
script adds selection, highlighting of connections, a details panel and step-by-step walks.
No external libraries or network access. Run with --check to fail if the file is out of date.
"""
import json
import math
import os
import sys
import textwrap
from html import escape

import yaml

import content as C

OUT = 'docs/system/explorer.html'
KIND_LABEL = {
    'apb': 'APB register bus', 'ahb': 'AHB-Lite bus (crypto)', 'axi4': 'AXI4 memory bus', 'axi4-stream': 'AXI4-Stream data',
    'irq': 'Interrupt', 'mem': 'Memory access', 'rpmsg': 'RPMsg message channel',
    'uses': 'Uses / depends on', 'runs': 'Runs on', 'boot': 'Boot hand-over',
}
LEGEND = ['apb', 'axi4', 'axi4-stream', 'irq', 'mem', 'rpmsg', 'boot', 'uses']


def load_layout():
    with open(os.path.join(C.DOCS, 'system', 'layout.yaml')) as f:
        return yaml.safe_load(f)


def clip(box, toward):
    """Point where the ray from the box centre toward `toward` leaves the box."""
    x, y, w, h = box[:4]
    cx, cy = x + w / 2, y + h / 2
    dx, dy = toward[0] - cx, toward[1] - cy
    if dx == 0 and dy == 0:
        return cx, cy
    t = min((w / 2) / abs(dx) if dx else 1e9, (h / 2) / abs(dy) if dy else 1e9)
    return cx + dx * t, cy + dy * t


def center(box):
    return box[0] + box[2] / 2, box[1] + box[3] / 2


def view_svg(view, arch):
    nodes = {n['id']: n for n in arch['nodes']}
    boxes = view['nodes']
    out = []
    out.append('<svg class="diagram" viewBox="0 0 1000 %d" role="group" aria-label="%s" data-view="%s">'
               % (view['height'], escape(view['title']), view['id']))
    out.append('<defs>')
    for k in KIND_LABEL:
        out.append('<marker id="a-%s-%s" class="mk mk-%s" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0 0L10 5L0 10z"/></marker>' % (view['id'], k, k))
    out.append('</defs>')
    for b in view.get('bands', []):
        x, y, w, h = b['box']
        out.append('<rect class="band" x="%d" y="%d" width="%d" height="%d" rx="6"/><text class="band-label" x="%d" y="%d">%s</text>'
                   % (x, y, w, h, x + 12, y + 16, escape(b['label'])))
    # edges first so boxes sit on top
    pair_count = {}
    for e in arch['edges']:
        if e['from'] in boxes and e['to'] in boxes:
            pair_count.setdefault(frozenset((e['from'], e['to'])), []).append(e['id'])
    for e in arch['edges']:
        if not (e['from'] in boxes and e['to'] in boxes):
            continue
        a, b = boxes[e['from']], boxes[e['to']]
        ca, cb = center(a), center(b)
        group = pair_count[frozenset((e['from'], e['to']))]
        idx = group.index(e['id'])
        off = (idx - (len(group) - 1) / 2) * 14
        dx, dy = cb[0] - ca[0], cb[1] - ca[1]
        norm = math.hypot(dx, dy) or 1
        px, py = -dy / norm * off, dx / norm * off
        ta = (cb[0] + px, cb[1] + py)
        tb = (ca[0] + px, ca[1] + py)
        p1 = clip(a, ta)
        p2 = clip(b, tb)
        p1 = (p1[0] + px, p1[1] + py)
        p2 = (p2[0] + px, p2[1] + py)
        d = 'M%.1f %.1fL%.1f %.1f' % (p1[0], p1[1], p2[0], p2[1])
        title = '%s: %s to %s. %s' % (KIND_LABEL[e['kind']], nodes[e['from']]['title'], nodes[e['to']]['title'], e['label'])
        out.append('<g class="edge k-%s" data-id="%s" data-from="%s" data-to="%s" tabindex="0" role="button" aria-label="%s">'
                   '<title>%s</title><path class="hit" d="%s"/><path class="line" d="%s" marker-end="url(#a-%s-%s)"/></g>'
                   % (e['kind'], e['id'], e['from'], e['to'], escape(title, True), escape(title), d, d, view['id'], e['kind']))
    for nid, box in boxes.items():
        n = nodes[nid]
        x, y, w, h = box[:4]
        label = box[4] if len(box) > 4 else n['title']
        lines = textwrap.wrap(label, width=max(8, int((w - 14) / 6.6)))[:3]
        text = ''.join('<tspan x="%.1f" dy="%s">%s</tspan>' % (x + w / 2, '0' if i == 0 else '1.15em', escape(t))
                       for i, t in enumerate(lines))
        ty = y + h / 2 - (len(lines) - 1) * 7.5 + 4.5
        out.append('<g class="node n-%s" data-id="%s" tabindex="0" role="button" aria-label="%s">'
                   '<rect x="%d" y="%d" width="%d" height="%d" rx="5"/><text x="%.1f" y="%.1f">%s</text></g>'
                   % (n['kind'], nid, escape(n['title'] + '. ' + n['summary'].split('. ')[0], True), x, y, w, h, x + w / 2, ty, text))
    out.append('</svg>')
    return '\n'.join(out)


def build_data(arch, mods, site=None):
    learn = {}
    for m in sorted(mods.values(), key=C.module_sort_key):
        for k in ('hardware', 'software'):
            for nid in m.get(k) or []:
                learn.setdefault(nid, []).append({
                    'id': m['id'], 'title': m['title'], 'status': m['status'],
                    'file': (site['url'](m['id']) if site else os.path.relpath(m['file'], 'docs/system')) if 'file' in m else None})
    nodes = {}
    for n in arch['nodes']:
        nodes[n['id']] = {
            'title': n['title'], 'kind': n['kind'], 'summary': n['summary'], 'parent': n.get('parent'),
            'facts': n.get('facts') or {},
            'sources': [{'path': s['path'], 'symbols': s.get('symbols') or []} for s in C.norm_sources(n.get('sources'))],
            'learn': learn.get(n['id'], []),
        }
    return {
        'baseline': arch['baseline'], 'nodes': nodes,
        'edges': {e['id']: e for e in arch['edges']},
        'paths': arch.get('paths', []), 'kinds': KIND_LABEL,
    }


CSS = """
:root{--bg:#f5f6f4;--surface:#ffffff;--ink:#1c2321;--muted:#5a6560;--line:#cfd6d2;--band:#ebeeec;
--hart:#d9e8f5;--hart-b:#2f6f9f;--mss:#e4e9f3;--mss-b:#5470a0;--fab:#e2f0e6;--fab-b:#3d7d54;--mem:#f1ece0;--mem-b:#8a7440;
--sw:#ffffff;--sw-b:#6a6f6c;--part:#eeeeee;--part-b:#80858a;--sel:#c2410c;
--c-apb:#4b6a8f;--c-axi4:#2f5f9f;--c-axi4-stream:#0f7a5c;--c-irq:#b4540a;--c-mem:#7a6a3a;--c-rpmsg:#7b4fa0;--c-boot:#1d1d1d;--c-uses:#8d9490;--c-runs:#8d9490}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){--bg:#14181a;--surface:#1c2225;--ink:#e6ece9;--muted:#98a39e;--line:#364047;--band:#192023;
--hart:#1f3a52;--hart-b:#6aa8d6;--mss:#222c40;--mss-b:#8da6d4;--fab:#1c3626;--fab-b:#6cbf88;--mem:#33301f;--mem-b:#c0a95f;
--sw:#1c2225;--sw-b:#8a928e;--part:#262d30;--part-b:#8a9298;--sel:#fb923c;
--c-apb:#8fb0d8;--c-axi4:#7fb0ee;--c-axi4-stream:#4fd1a5;--c-irq:#f59e5b;--c-mem:#cdb872;--c-rpmsg:#c1a0e4;--c-boot:#e6ece9;--c-uses:#7d8681;--c-runs:#7d8681;color-scheme:dark}}
:root[data-theme="dark"]{--bg:#14181a;--surface:#1c2225;--ink:#e6ece9;--muted:#98a39e;--line:#364047;--band:#192023;
--hart:#1f3a52;--hart-b:#6aa8d6;--mss:#222c40;--mss-b:#8da6d4;--fab:#1c3626;--fab-b:#6cbf88;--mem:#33301f;--mem-b:#c0a95f;
--sw:#1c2225;--sw-b:#8a928e;--part:#262d30;--part-b:#8a9298;--sel:#fb923c;
--c-apb:#8fb0d8;--c-axi4:#7fb0ee;--c-axi4-stream:#4fd1a5;--c-irq:#f59e5b;--c-mem:#cdb872;--c-rpmsg:#c1a0e4;--c-boot:#e6ece9;--c-uses:#7d8681;--c-runs:#7d8681;color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.5 "IBM Plex Sans",system-ui,-apple-system,"Segoe UI",sans-serif;
padding-left:max(16px,env(safe-area-inset-left,0px));padding-right:max(16px,env(safe-area-inset-right,0px));padding-block:20px 40px}
code,.mono{font-family:"IBM Plex Mono",ui-monospace,Menlo,Consolas,monospace;font-size:.92em}
header{max-width:1280px;margin:0 auto 14px}
h1{font-size:1.45rem;margin:0 0 4px;font-weight:600;text-wrap:balance}
.sub{color:var(--muted);margin:0;max-width:75ch}
.bar{max-width:1280px;margin:0 auto 12px;display:flex;flex-wrap:wrap;gap:10px 18px;align-items:center}
.tabs{display:flex;gap:4px;flex-wrap:wrap}
.theme-toggle,.tabs button,.walk button,.walk select{font:inherit;color:var(--ink);background:var(--surface);border:1px solid var(--line);border-radius:6px;padding:6px 12px;cursor:pointer}
.tabs button[aria-pressed="true"]{border-color:var(--sel);box-shadow:inset 0 -2px 0 var(--sel);font-weight:600}
.walk{display:flex;gap:6px;flex-wrap:wrap;align-items:center}
.walk button:disabled{opacity:.45;cursor:default}
button:focus-visible,.node:focus-visible,.edge:focus-visible,a:focus-visible,select:focus-visible{outline:2px solid var(--sel);outline-offset:2px}
.main{max-width:1280px;margin:0 auto;display:grid;grid-template-columns:minmax(0,1fr) 360px;gap:16px;align-items:start}
@media (max-width:900px){.main{grid-template-columns:minmax(0,1fr)}}
.stage{background:var(--surface);border:1px solid var(--line);border-radius:8px;padding:10px;min-width:0}
.diagram-wrap{overflow-x:auto}
svg.diagram{display:block;width:100%;min-width:640px;height:auto}
.band{fill:var(--band);stroke:var(--line)}
.band-label{fill:var(--muted);font-size:12px;letter-spacing:.04em;text-transform:uppercase}
.node{cursor:pointer}
.node rect{stroke-width:1.5}
.node text{font-size:13px;text-anchor:middle;fill:var(--ink);pointer-events:none}
.n-hart rect{fill:var(--hart);stroke:var(--hart-b)}
.n-mss-block rect,.n-bus rect{fill:var(--mss);stroke:var(--mss-b)}
.n-fabric-block rect{fill:var(--fab);stroke:var(--fab-b)}
.n-memory rect{fill:var(--mem);stroke:var(--mem-b)}
.n-software rect,.n-host rect{fill:var(--sw);stroke:var(--sw-b)}
.n-board-part rect,.n-group rect{fill:var(--part);stroke:var(--part-b)}
.edge{cursor:pointer}
.edge .hit{stroke:transparent;stroke-width:14;fill:none}
.edge .line{fill:none;stroke-width:1.6}
.k-apb .line,.k-ahb .line{stroke:var(--c-apb)}.k-ahb .line{stroke-dasharray:10 3}.k-axi4 .line{stroke:var(--c-axi4);stroke-width:2.2}
.k-axi4-stream .line{stroke:var(--c-axi4-stream);stroke-width:4.5}
.k-irq .line{stroke:var(--c-irq);stroke-dasharray:7 5}.k-mem .line{stroke:var(--c-mem);stroke-dasharray:2 4;stroke-width:2.4}
.k-rpmsg .line{stroke:var(--c-rpmsg);stroke-dasharray:9 3 2 3}.k-boot .line{stroke:var(--c-boot);stroke-width:2.6}
.k-uses .line,.k-runs .line{stroke:var(--c-uses);stroke-width:1.2}
.mk path{fill:var(--c-uses)}.mk-apb path,.mk-ahb path{fill:var(--c-apb)}.mk-axi4 path{fill:var(--c-axi4)}.mk-axi4-stream path{fill:var(--c-axi4-stream)}
.mk-irq path{fill:var(--c-irq)}.mk-mem path{fill:var(--c-mem)}.mk-rpmsg path{fill:var(--c-rpmsg)}.mk-boot path{fill:var(--c-boot)}
.has-sel .node,.has-sel .edge{opacity:.22;transition:opacity .15s}
.has-sel .node.on,.has-sel .edge.on{opacity:1}
.node.sel rect{stroke:var(--sel);stroke-width:3}
.edge.sel .line,.edge.step .line{stroke:var(--sel);stroke-width:4}
.edge.step .line{stroke-dasharray:none}
@media (prefers-reduced-motion:reduce){.has-sel .node,.has-sel .edge{transition:none}}
.legend{display:flex;flex-wrap:wrap;gap:6px 16px;margin:10px 4px 2px;font-size:.82rem;color:var(--muted)}
.legend span{display:inline-flex;align-items:center;gap:6px}
.legend svg{width:34px;height:10px}
.legend line{stroke-width:2}
aside{background:var(--surface);border:1px solid var(--line);border-radius:8px;padding:14px 16px;position:sticky;top:12px;max-height:calc(100vh - 24px);overflow:auto;min-width:0}
@media (max-width:900px){aside{position:static;max-height:none}}
aside h2{font-size:1.1rem;margin:0 0 2px;font-weight:600}
aside .kind{color:var(--muted);font-size:.8rem;text-transform:uppercase;letter-spacing:.05em;margin:0 0 8px}
aside h3{font-size:.78rem;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);margin:14px 0 4px;font-weight:600}
aside ul{margin:0;padding:0;list-style:none}
aside li{margin:2px 0}
aside .link{background:none;border:0;padding:0;color:var(--ink);text-decoration:underline;text-underline-offset:3px;cursor:pointer;font:inherit;text-align:left}
aside dl{margin:0;display:grid;grid-template-columns:auto 1fr;gap:2px 10px}
aside dt{color:var(--muted)}aside dd{margin:0;overflow-wrap:anywhere}
.step-box{border-left:3px solid var(--sel);padding:4px 10px;margin:8px 0}
.tag{font-size:.72rem;border:1px solid var(--line);border-radius:4px;padding:0 5px;color:var(--muted);margin-left:4px}
noscript p{max-width:1280px;margin:8px auto;color:var(--muted)}
footer{max-width:1280px;margin:18px auto 0;color:var(--muted);font-size:.85rem}
"""

JS = r"""
(function(){
var D=JSON.parse(document.getElementById('data').textContent);
var ROOT=D.root||(D.site?null:'../../');
var $=function(s,r){return (r||document).querySelector(s)},$$=function(s,r){return Array.prototype.slice.call((r||document).querySelectorAll(s))};
var stage=$('#stage'),panel=$('#panel'),tabs=$$('.tabs button'),sel=null,selKind=null,walk=null,stepI=0;
function el(t,c,x){var e=document.createElement(t);if(c)e.className=c;if(x!=null)e.textContent=x;return e}
function viewOf(id,isEdge){
  var views=$$('svg.diagram');
  for(var i=0;i<views.length;i++){var q=isEdge?'.edge[data-id="'+id+'"]':'.node[data-id="'+id+'"]';if($(q,views[i]))return views[i].getAttribute('data-view')}
  return null}
function showView(v){
  $$('svg.diagram').forEach(function(s){s.parentNode.hidden=s.getAttribute('data-view')!==v});
  tabs.forEach(function(b){b.setAttribute('aria-pressed',b.getAttribute('data-view')===v?'true':'false')});
  apply()}
function curView(){var t=tabs.filter(function(b){return b.getAttribute('aria-pressed')==='true'})[0];return t?t.getAttribute('data-view'):tabs[0].getAttribute('data-view')}
function connected(id){
  var ids={},eds={};ids[id]=1;
  Object.keys(D.edges).forEach(function(k){var e=D.edges[k];if(e.from===id||e.to===id){eds[k]=1;ids[e.from]=1;ids[e.to]=1}});
  return {ids:ids,eds:eds}}
function apply(){
  var svgs=$$('svg.diagram');
  svgs.forEach(function(s){
    $$('.on,.sel,.step',s).forEach(function(n){n.classList.remove('on','sel','step')});
    s.classList.remove('has-sel')});
  var on={ids:{},eds:{}},stepE=null;
  if(walk){var e=D.edges[walk.steps[stepI].edge];stepE=e.id;on.ids[e.from]=1;on.ids[e.to]=1;on.eds[e.id]=1}
  else if(sel&&selKind==='node'){on=connected(sel)}
  else if(sel&&selKind==='edge'){var e2=D.edges[sel];on.ids[e2.from]=1;on.ids[e2.to]=1;on.eds[sel]=1}
  var any=walk||sel;
  svgs.forEach(function(s){
    if(any)s.classList.add('has-sel');
    $$('.node',s).forEach(function(n){var id=n.getAttribute('data-id');if(on.ids[id])n.classList.add('on');if(sel===id&&selKind==='node'&&!walk)n.classList.add('sel')});
    $$('.edge',s).forEach(function(n){var id=n.getAttribute('data-id');if(on.eds[id])n.classList.add('on');if(sel===id&&selKind==='edge'&&!walk)n.classList.add('sel');if(stepE===id)n.classList.add('step')})})}
function link(txt,fn){var b=el('button','link',txt);b.type='button';b.onclick=fn;return b}
function go(id){selectNode(id,true)}
function selectNode(id,jump){
  walk=null;resetWalkUI();sel=id;selKind='node';
  if(jump){var v=viewOf(id,false);if(v&&v!==curView())showView(v)}
  apply();renderNode(id);history.replaceState(null,'','#'+id)}
function selectEdge(id){
  walk=null;resetWalkUI();sel=id;selKind='edge';apply();renderEdge(id);history.replaceState(null,'','#'+id)}
function clearSel(){sel=null;walk=null;resetWalkUI();apply();renderHome();history.replaceState(null,'',location.pathname+location.search)}
function renderHome(){
  panel.textContent='';
  panel.appendChild(el('h2',null,'Pick a component'));
  panel.appendChild(el('p',null,'Select a box to see what it does, what it connects to, its source files and the modules that explain it. Select a line to see what it carries. Or play a walk through the system.'));
  panel.appendChild(el('p','kind','Baseline '+D.baseline.tag+' · '+D.baseline.board+' · fabric v'+D.baseline.fabric))}
function renderNode(id){
  var n=D.nodes[id];panel.textContent='';
  panel.appendChild(el('h2',null,n.title));panel.appendChild(el('p','kind',n.kind.replace('-',' ')));
  panel.appendChild(el('p',null,n.summary));
  var f=Object.keys(n.facts);
  if(f.length){var dl=el('dl');f.forEach(function(k){dl.appendChild(el('dt',null,k.replace(/_/g,' ')));dl.appendChild(el('dd','mono',String(n.facts[k])))});panel.appendChild(el('h3',null,'Facts'));panel.appendChild(dl)}
  if(n.parent){panel.appendChild(el('h3',null,'Part of'));var u=el('ul'),li=el('li');li.appendChild(link(D.nodes[n.parent].title,function(){go(n.parent)}));u.appendChild(li);panel.appendChild(u)}
  var kids=Object.keys(D.nodes).filter(function(k){return D.nodes[k].parent===id});
  if(kids.length){panel.appendChild(el('h3',null,'Contains'));var uk=el('ul');kids.forEach(function(k){var l=el('li');l.appendChild(link(D.nodes[k].title,function(){go(k)}));uk.appendChild(l)});panel.appendChild(uk)}
  var out=[],inn=[];
  Object.keys(D.edges).forEach(function(k){var e=D.edges[k];if(e.from===id)out.push(e);if(e.to===id)inn.push(e)});
  [['Sends to',out,'to'],['Receives from',inn,'from']].forEach(function(g){
    if(!g[1].length)return;panel.appendChild(el('h3',null,g[0]));var ul=el('ul');
    g[1].forEach(function(e){var l=el('li');l.appendChild(link(D.nodes[e[g[2]]].title,function(){go(e[g[2]])}));l.appendChild(document.createTextNode(': '+e.label));l.appendChild(el('span','tag',D.kinds[e.kind]));ul.appendChild(l)});
    panel.appendChild(ul)});
  if(n.sources.length){panel.appendChild(el('h3',null,'Source'));var us=el('ul');n.sources.forEach(function(s){var l=el('li');var a=el(ROOT===null?'span':'a','mono',s.path);if(ROOT!==null)a.href=ROOT+s.path;l.appendChild(a);if(s.symbols.length)l.appendChild(document.createTextNode(' : '+s.symbols.join(', ')));us.appendChild(l)});panel.appendChild(us)}
  if(n.learn.length){panel.appendChild(el('h3',null,'Learn how this works'));var ul2=el('ul');n.learn.forEach(function(m){var l=el('li');
    if(m.file){var a=el('a',null,m.id+' '+m.title);a.href=m.file;l.appendChild(a)}else{l.appendChild(document.createTextNode(m.id+' '+m.title));l.appendChild(el('span','tag','planned'))}
    ul2.appendChild(l)});panel.appendChild(ul2)}
  if(!D.site){var all=el('p');var a2=el('a',null,'Component page');a2.href='README.md#'+id;all.appendChild(a2);panel.appendChild(all)}
  var cb=link('Clear selection',clearSel);panel.appendChild(cb)}
function renderEdge(id){
  var e=D.edges[id];panel.textContent='';
  panel.appendChild(el('h2',null,e.label));panel.appendChild(el('p','kind',D.kinds[e.kind]));
  if(e.detail)panel.appendChild(el('p',null,e.detail));
  var ul=el('ul');[['From',e.from],['To',e.to]].forEach(function(p){var l=el('li');l.appendChild(document.createTextNode(p[0]+': '));l.appendChild(link(D.nodes[p[1]].title,function(){go(p[1])}));ul.appendChild(l)});panel.appendChild(ul);
  var ps=D.paths.filter(function(p){return p.steps.some(function(s){return s.edge===id})});
  if(ps.length){panel.appendChild(el('h3',null,'Used in walks'));var u2=el('ul');ps.forEach(function(p){var l=el('li');l.appendChild(link(p.title,function(){startWalk(p.id)}));u2.appendChild(l)});panel.appendChild(u2)}
  panel.appendChild(link('Clear selection',clearSel))}
var wsel=$('#walk-select'),wprev=$('#walk-prev'),wnext=$('#walk-next'),wstop=$('#walk-stop');
function resetWalkUI(){if(wsel)wsel.value='';wprev.disabled=wnext.disabled=wstop.disabled=true}
function startWalk(pid){
  var p=D.paths.filter(function(x){return x.id===pid})[0];if(!p){resetWalkUI();return}
  walk=p;stepI=0;sel=null;wsel.value=pid;wprev.disabled=true;wnext.disabled=false;wstop.disabled=false;showStep()}
function showStep(){
  var s=walk.steps[stepI],e=D.edges[s.edge];
  var v=viewOf(e.id,true);if(v&&v!==curView())showView(v);
  apply();panel.textContent='';
  panel.appendChild(el('h2',null,walk.title));
  panel.appendChild(el('p','kind','Step '+(stepI+1)+' of '+walk.steps.length));
  var b=el('div','step-box');b.appendChild(el('p',null,s.say));panel.appendChild(b);
  var p=el('p');p.appendChild(link(D.nodes[e.from].title,function(){go(e.from)}));p.appendChild(document.createTextNode(' → '));p.appendChild(link(D.nodes[e.to].title,function(){go(e.to)}));
  p.appendChild(el('span','tag',D.kinds[e.kind]));panel.appendChild(p);
  wprev.disabled=stepI===0;wnext.disabled=stepI===walk.steps.length-1}
wsel.onchange=function(){if(wsel.value)startWalk(wsel.value);else clearSel()};
wprev.onclick=function(){if(walk&&stepI>0){stepI--;showStep()}};
wnext.onclick=function(){if(walk&&stepI<walk.steps.length-1){stepI++;showStep()}};
wstop.onclick=clearSel;
tabs.forEach(function(b){b.onclick=function(){showView(b.getAttribute('data-view'))}});
stage.addEventListener('click',function(ev){
  var t=ev.target.closest&&ev.target.closest('.node,.edge');
  if(!t){if(ev.target.closest&&ev.target.closest('svg.diagram'))clearSel();return}
  if(t.classList.contains('node'))selectNode(t.getAttribute('data-id'),false);else selectEdge(t.getAttribute('data-id'))});
stage.addEventListener('keydown',function(ev){
  if(ev.key!=='Enter'&&ev.key!==' ')return;
  var t=ev.target.closest&&ev.target.closest('.node,.edge');if(!t)return;ev.preventDefault();
  if(t.classList.contains('node'))selectNode(t.getAttribute('data-id'),false);else selectEdge(t.getAttribute('data-id'))});
document.addEventListener('keydown',function(ev){if(ev.key==='Escape')clearSel()});
function fromHash(){
  var h=location.hash.replace('#','');
  if(h&&D.nodes[h])selectNode(h,true);else if(h&&D.edges[h]){var v=viewOf(h,true);if(v)showView(v);selectEdge(h)}
  else if(h&&D.paths.some(function(p){return p.id===h}))startWalk(h);else{showView(curView());renderHome()}}
window.addEventListener('hashchange',fromHash);
$$('.js-only').forEach(function(x){x.hidden=false});
resetWalkUI();fromHash();
})();
(function(){
  var btn=document.getElementById('theme-toggle'),root=document.documentElement;
  function dark(){var a=root.getAttribute('data-theme');return a?a==='dark':window.matchMedia('(prefers-color-scheme: dark)').matches}
  function label(){btn.textContent=dark()?'Light mode':'Dark mode'}
  btn.onclick=function(){var t=dark()?'light':'dark';root.setAttribute('data-theme',t);try{localStorage.setItem('theme',t)}catch(e){}label()};
  label()})();

"""


def build_html(arch, layout, mods, site=None):
    data = build_data(arch, mods, site)
    data['site'] = bool(site)
    data['root'] = site.get('root') if site else None
    b = arch['baseline']
    views = layout['views']
    svgs = ''.join('<div class="diagram-wrap"%s>%s</div>' % ('' if i == 0 else ' hidden', view_svg(v, arch)) for i, v in enumerate(views))
    tabs = ''.join('<button type="button" data-view="%s" aria-pressed="%s">%s</button>' % (v['id'], 'true' if i == 0 else 'false', escape(v['title'])) for i, v in enumerate(views))
    walks = ''.join('<option value="%s">%s</option>' % (p['id'], escape(p['title'])) for p in arch.get('paths', []))
    legend = ''.join(
        '<span><svg viewBox="0 0 34 10" aria-hidden="true"><line class="k-%s" x1="1" y1="5" x2="33" y2="5" style="%s"/></svg>%s</span>'
        % (k, LEGEND_STYLE[k], escape(KIND_LABEL[k])) for k in LEGEND)
    if site:
        back = '<p class="sub"><a href="%s">Back to the explorer page</a></p>' % site['back']
        sysurl = site['system']
        foot = 'Every fact in this diagram comes from one data file, <a href="%s">architecture.yaml</a>. Source files link to the pfdaq repository.' % site['yaml']
    else:
        back = ''
        sysurl = 'README.md'
        foot = 'Evidence class of every fact: see <a href="../architecture.md">Architecture as built</a>. Source links are relative to the repository root.'
    return """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>pfdaq architecture explorer</title>
<script>try{var t=localStorage.getItem("theme");if(t==="light"||t==="dark"){document.documentElement.setAttribute("data-theme",t)}}catch(e){}</script>
<style>%s</style>
</head>
<body>
<header>
%s<h1>pfdaq architecture explorer</h1>
<p class="sub">%s on the %s (%s), fabric v%s. Select a component or a connection. Generated from <code>docs/system/architecture.yaml</code>; the same data drives the component pages.</p>
</header>
<div class="bar js-only" hidden>
<button type="button" id="theme-toggle" class="theme-toggle" aria-label="Switch between light and dark mode"></button>
<div class="tabs" role="group" aria-label="View">%s</div>
<div class="walk" role="group" aria-label="Walks through the system">
<label for="walk-select">Walk</label>
<select id="walk-select"><option value="">None</option>%s</select>
<button type="button" id="walk-prev">Previous</button><button type="button" id="walk-next">Next</button><button type="button" id="walk-stop">Stop</button>
</div>
</div>
<div class="main">
<div class="stage" id="stage">
%s
<div class="legend">%s</div>
</div>
<aside id="panel" aria-live="polite"><h2>Components</h2><p>The diagram is drawn as static SVG. Without JavaScript, the full description of every component is on the <a href="%s">system page</a>.</p></aside>
</div>
<footer>%s</footer>
<script id="data" type="application/json">%s</script>
<script>%s</script>
</body>
</html>
""" % (CSS, back, escape(b['tag']), escape(b['board']), escape(b['device']), escape(b['fabric']), tabs, walks, svgs, legend,
       sysurl, foot, json.dumps(data, separators=(',', ':')).replace('</', '<\\/'), JS)


LEGEND_STYLE = {
    'apb': 'stroke:var(--c-apb)', 'ahb': 'stroke:var(--c-apb);stroke-dasharray:10 3', 'axi4': 'stroke:var(--c-axi4)', 'axi4-stream': 'stroke:var(--c-axi4-stream);stroke-width:4',
    'irq': 'stroke:var(--c-irq);stroke-dasharray:7 5', 'mem': 'stroke:var(--c-mem);stroke-dasharray:2 4',
    'rpmsg': 'stroke:var(--c-rpmsg);stroke-dasharray:9 3 2 3', 'boot': 'stroke:var(--c-boot);stroke-width:3',
    'uses': 'stroke:var(--c-uses);stroke-width:1.2',
}


def problems(arch, layout):
    """Layout checks used by check_content.py."""
    errs = []
    ids = {n['id'] for n in arch['nodes']}
    for v in layout['views']:
        for nid in v['nodes']:
            if nid not in ids:
                errs.append('layout %s: unknown node %s' % (v['id'], nid))
    edges = {e['id']: e for e in arch['edges']}
    for p in arch.get('paths', []):
        for s in p['steps']:
            e = edges.get(s['edge'])
            if e and not any(e['from'] in v['nodes'] and e['to'] in v['nodes'] for v in layout['views']):
                errs.append('walk %s: edge %s cannot be shown in any view' % (p['id'], s['edge']))
    return errs


def main():
    arch = C.load_architecture()
    layout = load_layout()
    errs = problems(arch, layout)
    if errs:
        print('\n'.join(errs))
        return 1
    html = build_html(arch, layout, C.all_modules())
    full = os.path.join(C.ROOT, OUT)
    old = open(full).read() if os.path.isfile(full) else None
    if '--check' in sys.argv:
        if old != html:
            print('out of date: ' + OUT)
            return 1
        return 0
    with open(full, 'w') as f:
        f.write(html)
    print('%s: %d bytes' % (OUT, len(html)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
