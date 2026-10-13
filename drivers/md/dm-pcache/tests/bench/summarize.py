#!/usr/bin/env python3
"""
Walk results/<ts>/{ddmpcache,drbd_bcache}/*.json, extract the headline
numbers from each fio run, and write a markdown table comparing the two
stacks side by side.

Run as:
    summarize.py results/<timestamp>
Prints markdown to stdout and writes summary.csv into the result dir.
"""
import csv
import json
import sys
from pathlib import Path

WORKLOADS = [
    ('w1_randw_qd1_jobs1',   '4K randwrite qd=1  (latency)'),
    ('w2_randw_qd8_jobs1',   '4K randwrite qd=8  (concurrency)'),
    ('w3_seqw_128k_qd8',     '128K seqwrite qd=8 (bandwidth)'),
    ('w4_randr_qd32_jobs1',  '4K randread  qd=32 (cache hit)'),
]
STACKS = ['ddmpcache', 'drbd_bcache']


def extract(jpath):
    """Return dict with iops, bw_MBps, lat_avg_us, lat_p99_us, lat_p999_us."""
    if not jpath.exists():
        return None
    try:
        with open(jpath) as f:
            data = json.load(f)
    except (json.JSONDecodeError, OSError):
        return None
    jobs = data.get('jobs', [])
    if not jobs:
        return None
    # group_reporting: pick the first job that has the rw side we care about
    j = jobs[0]
    # Pick whichever side has non-zero IOPS
    for side in ('write', 'read'):
        s = j.get(side, {})
        if s.get('iops', 0) > 0:
            lat_ns = s.get('clat_ns', {})
            pct = lat_ns.get('percentile', {})
            return {
                'iops':       s.get('iops', 0),
                'bw_MBps':    s.get('bw', 0) / 1024.0,   # fio reports bw in KiB/s
                'lat_avg_us': lat_ns.get('mean', 0) / 1000.0,
                'lat_p99_us': pct.get('99.000000', 0) / 1000.0,
                'lat_p999_us': pct.get('99.900000', 0) / 1000.0,
            }
    return None


def fmt(v, unit=''):
    if v is None:
        return '-'
    if unit == 'iops':
        return f'{v:,.0f}'
    if unit == 'MBps':
        return f'{v:,.1f}'
    if unit == 'us':
        return f'{v:,.1f}'
    return str(v)


def main():
    if len(sys.argv) != 2:
        print('usage: summarize.py results/<timestamp>', file=sys.stderr)
        sys.exit(2)
    root = Path(sys.argv[1])
    if not root.is_dir():
        print(f'no such directory: {root}', file=sys.stderr)
        sys.exit(2)

    data = {}
    for s in STACKS:
        data[s] = {}
        for wid, _ in WORKLOADS:
            data[s][wid] = extract(root / s / f'{wid}.json')

    # CSV
    csv_path = root / 'summary.csv'
    with open(csv_path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['workload', 'stack', 'iops', 'bw_MBps',
                    'lat_avg_us', 'lat_p99_us', 'lat_p999_us'])
        for wid, _ in WORKLOADS:
            for s in STACKS:
                d = data[s][wid]
                if d:
                    w.writerow([wid, s, f"{d['iops']:.0f}",
                                f"{d['bw_MBps']:.1f}",
                                f"{d['lat_avg_us']:.1f}",
                                f"{d['lat_p99_us']:.1f}",
                                f"{d['lat_p999_us']:.1f}"])
                else:
                    w.writerow([wid, s, '-', '-', '-', '-', '-'])

    # Markdown
    out = []
    out.append(f'# Benchmark summary: pcache-dist vs DRBD+bcache')
    out.append(f'')
    out.append(f'Results directory: `{root}`')
    out.append(f'')
    out.append(f'Backing for both stacks: `/dev/nullb0` (zero-cost).  ')
    out.append(f'Replication: synchronous (Protocol C / dm-pcache sync).  ')
    out.append(f'fio --direct=1, 30 s per workload + 5 s ramp.')
    out.append(f'')

    headers = ['workload', 'stack', 'IOPS', 'BW MiB/s',
               'lat avg us', 'lat p99 us', 'lat p99.9 us']
    out.append('| ' + ' | '.join(headers) + ' |')
    out.append('|' + '|'.join(['---'] * len(headers)) + '|')
    for wid, label in WORKLOADS:
        for s in STACKS:
            d = data[s][wid]
            row = [label if s == STACKS[0] else '', s]
            if d:
                row += [fmt(d['iops'], 'iops'),
                        fmt(d['bw_MBps'], 'MBps'),
                        fmt(d['lat_avg_us'], 'us'),
                        fmt(d['lat_p99_us'], 'us'),
                        fmt(d['lat_p999_us'], 'us')]
            else:
                row += ['-'] * 5
            out.append('| ' + ' | '.join(str(x) for x in row) + ' |')

    out.append('')
    out.append('## Ratio (pcache-dist / drbd_bcache)')
    out.append('')
    out.append('| workload | IOPS x | BW x | lat avg x | lat p99 x |')
    out.append('|---|---|---|---|---|')
    for wid, label in WORKLOADS:
        a = data['ddmpcache'][wid]
        b = data['drbd_bcache'][wid]
        if not a or not b:
            out.append(f'| {label} | - | - | - | - |')
            continue
        def ratio(x, y):
            return f'{(x / y):.2f}' if y else '-'
        out.append('| {} | {} | {} | {} | {} |'.format(
            label,
            ratio(a['iops'], b['iops']),
            ratio(a['bw_MBps'], b['bw_MBps']),
            # latency: lower is better, so ddm/drbd < 1 means ddm wins
            ratio(a['lat_avg_us'], b['lat_avg_us']),
            ratio(a['lat_p99_us'], b['lat_p99_us']),
        ))

    print('\n'.join(out))


if __name__ == '__main__':
    main()
