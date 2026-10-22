#!/usr/bin/env python3
"""agent-set.py NAME STATUS ROLE TASK  - add or update an agent in bridge/agents.json (status: working|idle|done|waiting|blocked)."""
import json, os, sys
p = os.path.join(os.path.dirname(__file__), '..', '..', 'bridge', 'agents.json')
d = json.load(open(p)) if os.path.exists(p) else {'orchestrator': 'Claude', 'agents': []}
name, status, role, task = sys.argv[1:5]
for a in d['agents']:
    if a['name'] == name:
        a.update(status=status, role=role, task=task)
        break
else:
    d['agents'].append({'name': name, 'status': status, 'role': role, 'task': task})
json.dump(d, open(p, 'w'), ensure_ascii=False, indent=1)
