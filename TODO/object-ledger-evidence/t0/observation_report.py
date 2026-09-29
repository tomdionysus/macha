#!/usr/bin/env python3
"""Summarise a node's observations.jsonl (0.74.0 and later) for T0.

Usage: observation_report.py FILE [FILE...] [--since UNIX_MS] [--until UNIX_MS]

Merges every window's histogram buckets exactly (src/observation.hpp), so
the percentiles printed are over the whole soak, not an average of
per-window percentiles. Prints, per histogram series:

  - count, p50, p90, p99, max over all windows;
  - the same split by load class, where a window is "loaded" if the node
    committed FUSE publication bytes, ran a DATA retention barrier, or ran a
    repair step while a higher class was active in it, and "idle" otherwise;
  - the spread of hourly p50 and p99 (min, median, max over the hours with
    at least 20 values), which is the variance a threshold must clear.

Per counter: the total, and its rate per minute over the covered time.
Per cumulative gauge (repair_*, fuse_*): the rate between the first and last
window. Per sampled gauge (rss_bytes): min, median and max. Events are listed
in time order.
"""
import json
import math
import statistics
import sys
from collections import defaultdict

SUB = 8


def bucket_upper(b):
    if b < 8:
        return b
    octave = (b - 8) // SUB + 3
    sub = (b - 8) % SUB
    lower = (8 + sub) << (octave - 3)
    return lower + (1 << (octave - 3)) - 1


def quantile(buckets, count, q, cap):
    if count == 0:
        return 0
    rank = max(1, min(count, math.ceil(q * count)))
    seen = 0
    for b in sorted(buckets):
        seen += buckets[b]
        if seen >= rank:
            return min(bucket_upper(b), cap)
    return cap


class Series:
    def __init__(self):
        self.buckets = defaultdict(int)
        self.count = 0
        self.max = 0

    def add(self, h):
        for b, c in h["buckets"]:
            self.buckets[b] += c
        self.count += h["count"]
        self.max = max(self.max, h["max"])

    def summary(self):
        return {
            "count": self.count,
            "p50": quantile(self.buckets, self.count, 0.5, self.max),
            "p90": quantile(self.buckets, self.count, 0.9, self.max),
            "p99": quantile(self.buckets, self.count, 0.99, self.max),
            "max": self.max,
        }


def loaded(window, previous):
    counters = window.get("counters", {})
    if counters.get("claim.data_barrier.ids"):
        return True
    if any(k.startswith("maintenance.repair.bytes.loaded") for k in counters):
        return True
    if "maintenance.repair.step_us.loaded" in window.get("histograms", {}):
        return True
    if previous is not None:
        now = window.get("gauges", {}).get("fuse_publication_bytes_committed")
        then = previous.get("gauges", {}).get("fuse_publication_bytes_committed")
        if now is not None and then is not None and now > then:
            return True
    return False


def main(argv):
    files, since, until = [], 0, 1 << 63
    i = 0
    while i < len(argv):
        if argv[i] == "--since":
            since = int(argv[i + 1]); i += 2
        elif argv[i] == "--until":
            until = int(argv[i + 1]); i += 2
        else:
            files.append(argv[i]); i += 1
    windows, events = [], []
    for name in files:
        with open(name) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                record = json.loads(line)
                at = record.get("end_ms", record.get("at_ms", 0))
                if not (since <= at <= until):
                    continue
                (windows if record["kind"] == "window" else events).append(record)
    windows.sort(key=lambda w: w["end_ms"])
    events.sort(key=lambda e: e["at_ms"])
    if not windows:
        print("no windows")
        return

    overall = defaultdict(Series)
    by_class = {"idle": defaultdict(Series), "loaded": defaultdict(Series)}
    hourly = defaultdict(lambda: defaultdict(Series))
    counters = defaultdict(int)
    classes = defaultdict(int)
    previous = None
    for w in windows:
        cls = "loaded" if loaded(w, previous) else "idle"
        classes[cls] += 1
        hour = w["end_ms"] // 3_600_000
        for name, h in w.get("histograms", {}).items():
            overall[name].add(h)
            by_class[cls][name].add(h)
            hourly[name][hour].add(h)
        for name, v in w.get("counters", {}).items():
            counters[name] += v
        previous = w

    first, last = windows[0], windows[-1]
    minutes = max(1e-9, (last["end_ms"] - first["start_ms"]) / 60000)
    print(f"windows={len(windows)} idle={classes['idle']} loaded={classes['loaded']} "
          f"span_minutes={minutes:.0f} versions={sorted({w['version'] for w in windows})}")

    print("\nhistograms (microseconds unless named _ns)")
    for name in sorted(overall):
        s = overall[name].summary()
        line = f"  {name}: n={s['count']} p50={s['p50']} p90={s['p90']} p99={s['p99']} max={s['max']}"
        for cls in ("idle", "loaded"):
            if name in by_class[cls]:
                c = by_class[cls][name].summary()
                line += f" | {cls} n={c['count']} p50={c['p50']} p99={c['p99']}"
        hours = [h.summary() for h in hourly[name].values() if h.count >= 20]
        if len(hours) >= 2:
            p50s = sorted(h["p50"] for h in hours)
            p99s = sorted(h["p99"] for h in hours)
            line += (f" | hourly({len(hours)}) p50 {p50s[0]}/{statistics.median(p50s)}/{p50s[-1]}"
                     f" p99 {p99s[0]}/{statistics.median(p99s)}/{p99s[-1]}")
        print(line)

    print("\ncounters (total, per minute)")
    for name in sorted(counters):
        print(f"  {name}: {counters[name]} ({counters[name] / minutes:.2f}/min)")

    print("\ngauges")
    names = sorted({k for w in windows for k in w.get("gauges", {})})
    for name in names:
        values = [w["gauges"][name] for w in windows if name in w.get("gauges", {})]
        if name.startswith(("repair_", "fuse_publication", "fuse_publications", "maintenance_")):
            points = [(w["end_ms"], w["gauges"][name]) for w in windows if name in w.get("gauges", {})]
            span = max(1e-9, (points[-1][0] - points[0][0]) / 60000)
            print(f"  {name}: {points[0][1]} -> {points[-1][1]} "
                  f"({(points[-1][1] - points[0][1]) / span:.2f}/min, restarts reset it)")
        else:
            print(f"  {name}: min={min(values)} median={statistics.median(values)} max={max(values)}")

    print("\nevents")
    for e in events:
        print(f"  {e['at_ms']} {e['event']} {json.dumps(e.get('fields', {}), sort_keys=True)}")


if __name__ == "__main__":
    main(sys.argv[1:])
