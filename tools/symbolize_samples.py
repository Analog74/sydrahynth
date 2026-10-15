"""Per-module and per-function profile from a hydraboot HYDRA_SAMPLE file.

usage: python -I tools/symbolize_samples.py samples.txt   (needs addr2line)
"""
import sys, subprocess, collections, bisect
lines = open(sys.argv[1]).read().split('\n')
i = lines.index('SAMPLES')
maps = []
for l in lines[:i]:
    parts = l.split()
    if len(parts) < 5: continue
    lo, hi = (int(x, 16) for x in parts[0].split('-'))
    off = int(parts[2], 16)
    path = parts[5] if len(parts) > 5 else '[anon]'
    maps.append((lo, hi, off, path))
maps.sort()
samples = [int(x, 16) for x in lines[i+1:] if x]
by_module = collections.Counter()
per_file = collections.defaultdict(list)
for a in samples:
    for lo, hi, off, path in maps:
        if lo <= a < hi:
            by_module[path] += 1
            if not path.startswith('[') and '/' in path:
                per_file[path].append(a - lo + off)
            break
    else:
        by_module['?'] += 1
total = len(samples)
print('modules:')
for m, n in by_module.most_common(8):
    print(f'  {100*n/total:5.1f}%  {m}')
funcs = collections.Counter()
for path, addrs in per_file.items():
    out = subprocess.run(['addr2line', '-f', '-C', '-e', path] + [hex(a) for a in addrs], capture_output=True, text=True).stdout.split('\n')
    for k in range(0, len(out) - 1, 2):
        funcs[(path.split('/')[-1], out[k][:110])] += 1
print('functions:')
for (m, f), n in funcs.most_common(30):
    print(f'  {100*n/total:5.1f}%  {m}: {f}')
