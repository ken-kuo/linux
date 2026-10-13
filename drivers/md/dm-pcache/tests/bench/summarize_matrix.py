#!/usr/bin/env python3
"""
Summarize an interleaved matrix run: results/<ts>/<config>-r<round>/*.json
(bio_vs_dax.sh). For each workload print every config with the per-round
values and their median, so round-to-round spread stays visible.

Run as:
    summarize_matrix.py results/<timestamp>
"""
import re
import statistics
import sys
from pathlib import Path

from summarize import WORKLOADS, extract


def main():
    root = Path(sys.argv[1])
    runs = {}
    for d in sorted(p for p in root.iterdir() if p.is_dir()):
        m = re.match(r'(.+)-r(\d+)$', d.name)
        if m:
            runs.setdefault(m.group(1), []).append(d)
    order = [c for c in ('dax-solo', 'bio-solo', 'dax-repl', 'bio-repl') if c in runs]
    order += [c for c in runs if c not in order]

    for wid, desc in WORKLOADS:
        bw = wid.startswith('w3')
        key, unit = ('bw_MBps', 'MiB/s') if bw else ('iops', 'IOPS')
        print(f'\n### {desc}\n')
        print(f'| config | {unit} per round | median {unit} | median lat avg (us) | median p99 (us) |')
        print('|---|---|---|---|---|')
        for c in order:
            vals = [extract(d / f'{wid}.json') for d in runs[c]]
            vals = [v for v in vals if v]
            if not vals:
                print(f'| {c} | - | - | - | - |')
                continue
            per = ', '.join(f'{v[key]:,.0f}' for v in vals)
            print(f'| {c} | {per} | {statistics.median(v[key] for v in vals):,.0f} '
                  f'| {statistics.median(v["lat_avg_us"] for v in vals):,.1f} '
                  f'| {statistics.median(v["lat_p99_us"] for v in vals):,.1f} |')


if __name__ == '__main__':
    main()
