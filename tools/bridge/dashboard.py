#!/usr/bin/env python3
"""Local dashboard: who does what in the venus port (agents, bridge, CI, phone, last boot log).

  python3 tools/bridge/dashboard.py        -> http://127.0.0.1:8765   (reads only; writes nothing)

Agent roster: bridge/agents.json, edited with tools/bridge/agent-set.py.
"""
import glob, json, os, re, sqlite3, subprocess, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
PORT = 8765
REPO = 'Log1nYT/ios-sweet'
_cache = {}


def sh(cmd, ttl=0, timeout=15):
    now = time.time()
    hit = _cache.get(cmd)
    if hit and now - hit[0] < ttl:
        return hit[1]
    try:
        out = subprocess.run(cmd, shell=True, cwd=ROOT, capture_output=True, text=True, timeout=timeout).stdout.strip()
    except Exception as e:  # a slow tool must not take the page down
        out = 'error: %s' % e
    _cache[cmd] = (now, out)
    return out


def head(path, n=700):
    try:
        return open(path, errors='replace').read(n)
    except OSError:
        return ''


def mailbox():
    boxes = {}
    for d in ('to-opencode', 'work', 'to-claude', 'done'):
        items = []
        for f in sorted(glob.glob(os.path.join(ROOT, 'bridge', d, '*.md')), key=os.path.getmtime, reverse=True)[:8]:
            txt = head(f)
            items.append({'file': os.path.basename(f), 'mtime': int(os.path.getmtime(f)),
                          'from': (re.search(r'^From: (.*)$', txt, re.M) or [None, ''])[1],
                          'kind': (re.search(r'^Kind: (.*)$', txt, re.M) or [None, ''])[1],
                          'text': txt.split('\n\n', 1)[-1][:260]})
        boxes[d] = items
    return boxes


def agents():
    try:
        return json.load(open(os.path.join(ROOT, 'bridge', 'agents.json')))
    except Exception:
        return {'orchestrator': 'Claude', 'agents': []}


def phone():
    fb = sh('fastboot devices 2>/dev/null', 4)
    adb = sh('adb get-state 2>/dev/null', 4)
    ping = sh('ping -c1 -t1 192.168.7.2 >/dev/null 2>&1 && echo up || echo down', 4)
    if 'fastboot' in fb:
        state = 'fastboot'
    elif adb in ('recovery', 'device', 'sideload'):
        state = 'adb: ' + adb
    elif ping == 'up':
        state = 'iOS running (ping 192.168.7.2)'
    else:
        state = 'not connected / off'
    return state


def bootlog():
    logs = sorted(glob.glob(os.path.join(ROOT, 'logs', 'venus-live-*.txt')), key=os.path.getmtime)
    if not logs:
        return {}
    p = logs[-1]
    txt = open(p, errors='replace').read()
    g = lambda pat: len(re.findall(pat, txt, re.M))
    return {'file': os.path.basename(p), 'age_s': int(time.time() - os.path.getmtime(p)), 'lines': txt.count('\n'),
            'springboard_exec': g(r'^exec SpringBoard'), 'springboard_crash': g(r'^CRASH SpringBoard\['),
            'surface_create': g(r'SURF: backboardd create'), 'panic': g(r'SWEET PANIC: [^n]'),
            'hfs_errors': g(r'hfs_mark_inconsistent')}


def ci():
    out = sh("gh run list -R %s --workflow xnu.yml --limit 4 --json databaseId,status,conclusion,createdAt,headSha,event" % REPO, 15)
    try:
        return [{'id': r['databaseId'], 'status': r['status'], 'conclusion': r['conclusion'], 'created': r['createdAt'],
                 'sha': r['headSha'][:7], 'event': r['event']} for r in json.loads(out)]
    except Exception:
        return []


def opencode_sessions():
    db = os.path.expanduser('~/.local/share/opencode/opencode.db')
    try:
        c = sqlite3.connect('file:%s?mode=ro' % db, uri=True, timeout=1)
        rows = c.execute("select title, time_updated from session where title like 'bridge:%' order by time_updated desc limit 5").fetchall()
        return [{'title': t, 'updated': int(u / 1000) if u > 1e11 else int(u)} for t, u in rows]
    except Exception:
        return []


def state():
    return {'now': int(time.time()), 'orchestrator': agents().get('orchestrator'), 'agents': agents().get('agents', []),
            'phone': phone(), 'ci': ci(), 'bootlog': bootlog(), 'mailbox': mailbox(),
            'worker_up': bool(sh('pgrep -f oc-worker.sh', 4)), 'opencode': opencode_sessions(),
            'git': sh('git log -6 --format="%h|%s|%cr"', 8).splitlines()}


PAGE = r'''<!doctype html><html lang="uk"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Venus Agents</title><style>
:root{--bg:#f6f7f9;--card:#fff;--ink:#1c2230;--mute:#667085;--line:#e3e6ec;--ok:#16794c;--warn:#b25e09;--bad:#b42318;--acc:#2f5bea;--chip:#eef1f6}
@media(prefers-color-scheme:dark){:root{--bg:#10131a;--card:#181c26;--ink:#e8ebf2;--mute:#98a2b3;--line:#272d3b;--ok:#5fd39b;--warn:#f0b35c;--bad:#ff8a80;--acc:#8aa4ff;--chip:#222839}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.45 -apple-system,system-ui,sans-serif}
main{max-width:1180px;margin:0 auto;padding:20px 16px 48px}h1{font-size:20px;margin:0 0 2px}h2{font-size:13px;text-transform:uppercase;letter-spacing:.06em;color:var(--mute);margin:22px 0 8px}
.sub{color:var(--mute);margin-bottom:14px}.grid{display:grid;gap:10px;grid-template-columns:repeat(auto-fill,minmax(270px,1fr))}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px}.card.orch{border-color:var(--acc);box-shadow:0 0 0 1px var(--acc) inset}
.row{display:flex;justify-content:space-between;gap:8px;align-items:center}.name{font-weight:650}.task{color:var(--mute);margin-top:4px;font-size:13px}
.chip{display:inline-block;padding:1px 8px;border-radius:99px;background:var(--chip);font-size:12px;color:var(--mute)}
.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}.acc{color:var(--acc)}.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:currentColor;margin-right:6px}
table{width:100%;border-collapse:collapse}td{padding:4px 6px;border-bottom:1px solid var(--line);vertical-align:top}td:first-child{white-space:nowrap}
.mail{display:grid;gap:10px;grid-template-columns:repeat(auto-fit,minmax(240px,1fr))}.msg{border-top:1px solid var(--line);padding:6px 0;font-size:12.5px}.msg b{font-weight:600}.msg div{color:var(--mute);word-break:break-word}
code{font:12px ui-monospace,Menlo,monospace}</style></head><body><main>
<h1>Venus: агенти та процес</h1><div class="sub" id="sub"></div>
<h2>Оркестратор і агенти</h2><div class="grid" id="agents"></div>
<h2>Конвеєр</h2><div class="grid" id="pipe"></div>
<h2>Міст Claude ⇄ OpenCode</h2><div class="mail" id="mail"></div>
<h2>Коміти</h2><div class="card"><table id="git"></table></div>
</main><script>
const esc=s=>String(s??'').replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
const ago=(t,now)=>{let d=Math.max(0,now-t);return d<90?d+' с':d<5400?Math.round(d/60)+' хв':Math.round(d/3600)+' год'};
const cls=s=>({working:'warn',running:'warn',idle:'acc',done:'ok',waiting:'acc',blocked:'bad'}[s]||'');
async function tick(){try{const s=await (await fetch('/api/state')).json();const now=s.now;
document.getElementById('sub').textContent='оновлено '+new Date(now*1000).toLocaleTimeString()+' · оркестратор: '+s.orchestrator;
document.getElementById('agents').innerHTML=s.agents.map(a=>`<div class="card ${a.name===s.orchestrator?'orch':''}"><div class="row"><span class="name">${esc(a.name)}</span><span class="${cls(a.status)}"><span class="dot"></span>${esc(a.status)}</span></div><div><span class="chip">${esc(a.role)}</span></div><div class="task">${esc(a.task)}</div></div>`).join('');
const b=s.bootlog,c=s.ci[0];
document.getElementById('pipe').innerHTML=`
<div class="card"><div class="name">Телефон</div><div class="task">${esc(s.phone)}</div></div>
<div class="card"><div class="name">CI (xnu)</div>${s.ci.map(r=>`<div class="task"><span class="${r.status==='completed'?(r.conclusion==='success'?'ok':'bad'):'warn'}">${esc(r.status==='completed'?r.conclusion:r.status)}</span> · #${r.id} · ${esc(r.sha)} · ${esc(r.event)}</div>`).join('')||'<div class="task">—</div>'}</div>
<div class="card"><div class="name">Останній лог завантаження</div>${b.file?`<div class="task"><code>${esc(b.file)}</code> · ${b.lines} рядків · ${ago(now-b.age_s,now)} тому</div>
<div class="task">SpringBoard: запусків ${b.springboard_exec}, падінь <span class="${b.springboard_crash?'bad':'ok'}">${b.springboard_crash}</span> · поверхонь ${b.surface_create}</div>
<div class="task">паніки <span class="${b.panic?'bad':'ok'}">${b.panic}</span> · помилки HFS <span class="${b.hfs_errors?'bad':'ok'}">${b.hfs_errors}</span></div>`:'<div class="task">—</div>'}</div>
<div class="card"><div class="name">OpenCode воркер</div><div class="task"><span class="${s.worker_up?'ok':'bad'}">${s.worker_up?'працює':'зупинений'}</span></div>${s.opencode.map(o=>`<div class="task">${esc(o.title)} · ${ago(o.updated,now)} тому</div>`).join('')}</div>`;
const names={'to-opencode':'→ OpenCode (черга)','work':'у роботі','to-claude':'← Claude (вхідні)','done':'виконано'};
document.getElementById('mail').innerHTML=Object.keys(names).map(k=>`<div class="card"><div class="name">${names[k]} <span class="chip">${s.mailbox[k].length}</span></div>${s.mailbox[k].map(m=>`<div class="msg"><b>${esc(m.file)}</b> <span class="chip">${esc(m.from)} ${esc(m.kind)}</span> · ${ago(m.mtime,now)}<div>${esc(m.text)}</div></div>`).join('')||'<div class="task">порожньо</div>'}</div>`).join('');
document.getElementById('git').innerHTML=s.git.map(l=>{const [h,m,w]=l.split('|');return `<tr><td><code>${esc(h)}</code></td><td>${esc(m)}</td><td class="task">${esc(w)}</td></tr>`}).join('');
}catch(e){document.getElementById('sub').textContent='немає зв\'язку з сервером'}}
tick();setInterval(tick,5000);
</script></body></html>'''


class H(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith('/api/state'):
            body, ctype = json.dumps(state()).encode(), 'application/json'
        else:
            body, ctype = PAGE.encode(), 'text/html; charset=utf-8'
        self.send_response(200)
        self.send_header('Content-Type', ctype)
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


if __name__ == '__main__':
    print('http://127.0.0.1:%d' % PORT)
    ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()
