#!/usr/bin/env python3
# Parse trace_printk output from the PCACHE_LAT_TRACE instrumentation and
# report per-segment latency statistics (ns -> us). One trace file per side.
#
# Primary  lines:  P blk=.. sz=.. local=.. qwait=.. sendack=.. total=..
# Secondary lines: S blk=.. sz=.. recv=.. commit=.. acksend=.. total=..
#
# Usage: parse_lat_trace.py <results-dir>
import re, sys, glob, os, statistics as st

KV = re.compile(r'(\w+)=(\d+)')
# Segment fields per side, in path order (excluding blk/sz).
FIELDS = {
    'primary':   ['local', 'qwait', 'sendack', 'total'],
    'secondary': ['recv', 'commit', 'acksend', 'total'],
}

def pct(xs, p):
    if not xs: return 0
    xs = sorted(xs); k = int(round((p/100)*(len(xs)-1)))
    return xs[k]

def parse(path, side):
    tag = 'P blk=' if side == 'primary' else 'S blk='
    rows = []
    for line in open(path, errors='replace'):
        i = line.find(tag)
        if i < 0: continue
        kv = dict((k, int(v)) for k, v in KV.findall(line[i:]))
        rows.append(kv)
    return rows

def report(side, rows):
    print(f"\n=== {side}  (n={len(rows)} IOs) ===")
    if not rows:
        print("  (no samples)"); return
    sz = rows[0].get('sz', 0)
    print(f"  block size: {sz} bytes")
    print(f"  {'segment':<10} {'mean_us':>9} {'p50_us':>9} {'p99_us':>9} {'max_us':>9}")
    for f in FIELDS[side]:
        xs = [r[f] for r in rows if f in r]
        if not xs: continue
        print(f"  {f:<10} {st.mean(xs)/1000:>9.1f} {pct(xs,50)/1000:>9.1f} "
              f"{pct(xs,99)/1000:>9.1f} {max(xs)/1000:>9.1f}")

def main():
    d = sys.argv[1]
    for label in ('qd1', 'qd8'):
        print(f"\n########## workload: 128K seq write {label} ##########")
        for side in ('primary', 'secondary'):
            f = os.path.join(d, f"{side}_{label}.trace")
            if os.path.exists(f):
                report(side, parse(f, side))
            else:
                print(f"\n=== {side}: {f} not found ===")

if __name__ == '__main__':
    main()
