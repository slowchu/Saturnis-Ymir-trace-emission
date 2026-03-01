"""Bus trace analyzer - run against a converted JSONL trace file.
Usage: python analyze_trace.py <path_to_trace.jsonl>
"""
import json
import sys
from collections import Counter

def analyze(path):
    total = 0
    retries_nonzero = 0
    max_retries = 0
    max_tick_diff = 0
    contention_records = []

    masters = Counter()
    kinds = Counter()
    service_cycles_dist = Counter()

    tick_min = float('inf')
    tick_max = 0

    dma_count = 0
    dma_addr_min = float('inf')
    dma_addr_max = 0

    with open(path, 'r') as f:
        for line in f:
            rec = json.loads(line)
            total += 1

            t1 = rec['tick_first_attempt']
            t2 = rec['tick_complete']
            diff = t2 - t1
            retries = rec['retries']
            master = rec['master']
            kind = rec['kind']
            sc = rec['service_cycles']
            addr = int(rec['addr'], 16)

            tick_min = min(tick_min, t1)
            tick_max = max(tick_max, t2)

            masters[master] += 1
            kinds[kind] += 1
            service_cycles_dist[sc] += 1

            if diff > max_tick_diff:
                max_tick_diff = diff

            if retries > 0:
                retries_nonzero += 1
                max_retries = max(max_retries, retries)
                if len(contention_records) < 20:
                    contention_records.append(rec)

            if master == 'DMA':
                dma_count += 1
                dma_addr_min = min(dma_addr_min, addr)
                dma_addr_max = max(dma_addr_max, addr)

            if total % 5_000_000 == 0:
                print(f"  ...processed {total:,} records", file=sys.stderr)

    saturn_mhz = 28.636
    tick_span = tick_max - tick_min
    real_time_us = tick_span / saturn_mhz if saturn_mhz else 0
    real_time_ms = real_time_us / 1000

    print("=" * 60)
    print("BUS TRACE ANALYSIS")
    print("=" * 60)
    print(f"Total records:        {total:,}")
    print(f"Tick range:           {tick_min:,} - {tick_max:,} (span: {tick_span:,})")
    print(f"Approx real time:     {real_time_ms:,.2f} ms ({real_time_us:,.0f} us)")
    print()

    print("--- Masters ---")
    for m, c in masters.most_common():
        print(f"  {m:8s}: {c:>12,} ({100*c/total:.1f}%)")
    print()

    print("--- Access kinds ---")
    for k, c in kinds.most_common():
        print(f"  {k:12s}: {c:>12,} ({100*c/total:.1f}%)")
    print()

    print("--- Service cycles distribution ---")
    for sc, c in sorted(service_cycles_dist.items()):
        print(f"  {sc} cycles: {c:>12,} ({100*c/total:.1f}%)")
    print()

    print("--- Contention ---")
    print(f"  Records with retries > 0: {retries_nonzero:,} ({100*retries_nonzero/total:.3f}%)")
    print(f"  Max retries:              {max_retries}")
    print(f"  Max tick difference:      {max_tick_diff}")
    print()

    if contention_records:
        print("--- First contention records ---")
        for rec in contention_records:
            print(f"  seq={rec['seq']} master={rec['master']} addr={rec['addr']} "
                  f"ticks={rec['tick_first_attempt']}->{rec['tick_complete']} "
                  f"retries={rec['retries']} kind={rec['kind']}")
        print()

    print("--- DMA ---")
    print(f"  DMA records:   {dma_count:,}")
    if dma_count > 0:
        print(f"  DMA addr range: 0x{dma_addr_min:08X} - 0x{dma_addr_max:08X}")
    print()

if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <trace.jsonl>")
        sys.exit(1)
    analyze(sys.argv[1])
