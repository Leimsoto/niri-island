#!/usr/bin/env python3
"""Tiempo de GPU por proceso durante N segundos (por defecto 3).

Lee drm-engine-* de /proc/<pid>/fdinfo (lo publican i915, xe, amdgpu y
otros drivers DRM): qué proceso ocupa la GPU de verdad, en % del intervalo.
    tools/gpu-procs.py 4
"""
import os
import sys
import time


def sample():
    out = {}
    for pid in filter(str.isdigit, os.listdir('/proc')):
        try:
            clients = {}
            for fd in os.listdir(f'/proc/{pid}/fdinfo'):
                try:
                    txt = open(f'/proc/{pid}/fdinfo/{fd}').read()
                except OSError:
                    continue
                if 'drm-engine-' not in txt:
                    continue
                kv = dict(line.split(':', 1) for line in txt.splitlines() if ':' in line)
                cid = kv.get('drm-client-id', '').strip()
                clients[cid] = sum(int(v.split()[0]) for k, v in kv.items()
                                   if k.startswith('drm-engine-') and v.split()[0].isdigit())
            if clients:
                out[pid] = (open(f'/proc/{pid}/comm').read().strip(), sum(clients.values()))
        except OSError:
            pass
    return out


n = float(sys.argv[1]) if len(sys.argv) > 1 else 3
a = sample()
t = time.monotonic()
time.sleep(n)
b = sample()
dt = time.monotonic() - t
rows = [(100 * (b[p][1] - a[p][1]) / (dt * 1e9), b[p][0], p) for p in b if p in a]
for pct, name, pid in sorted(rows, reverse=True)[:8]:
    if pct >= 0.5:
        print(f'{pct:5.1f}%  {name} ({pid})')
