#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproduction and measurement harness for the loader-I/O starvation P0.

Measures whether a node keeps its control plane responsive while its own
loader work runs. See TODO/2026-09-19-governing-laws-on-disk-io-plan.md.

It samples control-plane service time from two vantage points at once --
on the node against the server's own port, and from here through whatever
sits in front of it -- because those two answer different questions. If the
node is fast and the far side is slow, the latency is not in the server.

Node vitals (load, iowait, per-process disk bytes) are sampled on the same
clock so a latency excursion can be attributed rather than guessed at.

Observes by default. --generate-load writes through the FUSE mount to
produce the loader I/O itself; without it the harness measures whatever the
node already happens to be doing.

  ./run-io-pressure.py --node root@10.34.1.50 \
      --url https://ramaroja.macha.network --duration 120 --out before.json
  ./run-io-pressure.py --compare before.json after.json
"""

import argparse
import json
import math
import re
import statistics
import http.client
import ssl
import subprocess
import sys
import threading
import time
from urllib.parse import urlsplit

HEALTH = "/api/v1/health"

# The acceptance criteria from the plan, stage 1.
TARGET_P99_MS = 250.0
TARGET_MAX_MS = 1000.0


def pct(values, q):
    if not values:
        return None
    s = sorted(values)
    if len(s) == 1:
        return s[0]
    k = (len(s) - 1) * q
    lo, hi = math.floor(k), math.ceil(k)
    return s[lo] if lo == hi else s[lo] + (s[hi] - s[lo]) * (k - lo)


def summarise(samples):
    """samples: list of (monotonic_ts, latency_ms or None for failure)."""
    ok = [ms for _, ms in samples if ms is not None]
    fail = sum(1 for _, ms in samples if ms is None)
    if not ok:
        return {"count": len(samples), "failed": fail}
    return {
        "count": len(samples),
        "failed": fail,
        "p50_ms": round(pct(ok, 0.50), 2),
        "p95_ms": round(pct(ok, 0.95), 2),
        "p99_ms": round(pct(ok, 0.99), 2),
        "max_ms": round(max(ok), 2),
        "mean_ms": round(statistics.fmean(ok), 2),
        "over_250ms": sum(1 for v in ok if v > 250),
        "over_1s": sum(1 for v in ok if v > 1000),
    }


class ExternalProbe(threading.Thread):
    """Samples the public endpoint over ONE persistent connection.

    A fresh connection per sample measures the TCP+TLS+HTTP/2 handshake, not
    the server. Against a node ~60 ms away that is a ~420 ms floor on an idle
    node, which swamps the signal entirely. The real client holds a keep-alive
    connection, so the probe must too.
    """

    def __init__(self, url, interval, stop, timeout):
        super().__init__(daemon=True)
        self.name_ = "external"
        self.interval = interval
        self.stop = stop
        self.timeout = timeout
        self.samples = []
        self.reconnects = 0
        u = urlsplit(url)
        self.host = u.hostname
        self.port = u.port or (443 if u.scheme == "https" else 80)
        self.https = u.scheme == "https"
        self.conn = None

    def _connect(self):
        if self.conn:
            try:
                self.conn.close()
            except Exception:
                pass
        if self.https:
            self.conn = http.client.HTTPSConnection(
                self.host, self.port, timeout=self.timeout,
                context=ssl.create_default_context())
        else:
            self.conn = http.client.HTTPConnection(
                self.host, self.port, timeout=self.timeout)
        self.conn.connect()

    def run(self):
        t0 = time.monotonic()
        try:
            self._connect()
        except Exception:
            self.conn = None
        while not self.stop.is_set():
            started = time.monotonic()
            ms = None
            for attempt in (0, 1):
                try:
                    if self.conn is None:
                        self._connect()
                        if attempt:
                            self.reconnects += 1
                    t = time.monotonic()
                    self.conn.request("GET", f"{HEALTH}?_={int(time.time()*1000)}",
                                      headers={"Connection": "keep-alive"})
                    r = self.conn.getresponse()
                    r.read()
                    ms = (time.monotonic() - t) * 1000.0
                    break
                except Exception:
                    # A dropped keep-alive is not a latency sample; retry once
                    # on a fresh connection, and only then call it a failure.
                    self.conn = None
            self.samples.append((round(started - t0, 3), ms))
            slack = self.interval - (time.monotonic() - started)
            if slack > 0:
                self.stop.wait(slack)
        if self.conn:
            try:
                self.conn.close()
            except Exception:
                pass


REMOTE_VITALS = r'''
PID=$(pgrep -xo macha)
AUTH=""
if [ -n "PROBEUSER" ]; then
  printf '{"credentials":{"username":"%s","password":"%s"}}' "PROBEUSER" "PROBEPASS" \
    > /tmp/.machaprobe.$$
  TOK=$(curl -s -X POST -H "Content-Type: application/json" \
        --data-binary @/tmp/.machaprobe.$$ \
        http://127.0.0.1:PORT/api/v1/session |
        sed -n 's/.*"token":"\([0-9a-f]*\)".*/\1/p')
  rm -f /tmp/.machaprobe.$$
  [ -n "$TOK" ] && AUTH="Authorization: Bearer $TOK"
  echo "AUTH $( [ -n "$TOK" ] && echo ok || echo FAILED )"
fi
STREAM=""
SIZE=0
if [ -n "$AUTH" ] && [ -n "MEDIAPATH" ]; then
  printf '{"media_id":"path:%s","preferences":{"mode":"direct"}}' "MEDIAPATH" \
    > /tmp/.machasess.$$
  RESP=$(curl -s -X POST -H "Content-Type: application/json" -H "$AUTH" \
         --data-binary @/tmp/.machasess.$$ \
         http://127.0.0.1:PORT/api/v1/playback/sessions)
  rm -f /tmp/.machasess.$$
  STREAM=$(printf '%s' "$RESP" | sed -n 's/.*"url":"\([^"]*\)".*/\1/p' | head -1)
  SIZE=$(printf '%s' "$RESP" | sed -n 's/.*"size":\([0-9]*\).*/\1/p' | head -1)
  [ -z "$SIZE" ] && SIZE=0
  echo "VIEWER $( [ -n "$STREAM" ] && [ "$SIZE" -gt 0 ] && echo ok || echo FAILED ) $SIZE"
fi
prev_total=0; prev_idle=0; prev_wait=0
read _ u n s i w rest < /proc/stat
prev_idle=$i; prev_wait=$w
prev_total=$((u+n+s+i+w))
prev_r=$(awk '/^read_bytes/{print $2}' /proc/$PID/io)
prev_w=$(awk '/^write_bytes/{print $2}' /proc/$PID/io)
END=$(( $(date +%s) + DURATION ))
while [ "$(date +%s)" -lt "$END" ]; do
  sleep 1
  read _ u n s i w rest < /proc/stat
  total=$((u+n+s+i+w))
  dt=$((total-prev_total)); di=$((i-prev_idle)); dw=$((w-prev_wait))
  prev_total=$total; prev_idle=$i; prev_wait=$w
  r=$(awk '/^read_bytes/{print $2}' /proc/$PID/io)
  wb=$(awk '/^write_bytes/{print $2}' /proc/$PID/io)
  dr=$(( (r-prev_r)/1048576 )); dwb=$(( (wb-prev_w)/1048576 ))
  prev_r=$r; prev_w=$wb
  la=$(cut -d' ' -f1 /proc/loadavg)
  [ "$dt" -gt 0 ] || dt=1
  echo "VITALS $(date +%s) $la $(( dw*100/dt )) $(( di*100/dt )) $dr $dwb"
  t=$(curl -s -o /dev/null -w '%{time_total}' --max-time TIMEOUT \
        http://127.0.0.1:PORT/api/v1/health 2>/dev/null) || t=""
  echo "PROBE $(date +%s) ${t:-FAIL}"
  if [ -n "$AUTH" ]; then
    d=$(curl -s -o /dev/null -w '%{time_total}' --max-time TIMEOUT \
          -H "$AUTH" http://127.0.0.1:PORTDATAPATH 2>/dev/null) || d=""
  else
    d=$(curl -s -o /dev/null -w '%{time_total}' --max-time TIMEOUT \
          http://127.0.0.1:PORTDATAPATH 2>/dev/null) || d=""
  fi
  echo "DPROBE $(date +%s) ${d:-FAIL}"
  if [ -n "$STREAM" ] && [ "$SIZE" -gt 2000000 ]; then
    OFF=$(( (RANDOM * 32768 + RANDOM) % (SIZE - 1048576) ))
    v=$(curl -s -o /dev/null -w '%{time_total}' --max-time TIMEOUT \
          -H "Range: bytes=$OFF-$((OFF + 1048575))" \
          "http://127.0.0.1:PORT$STREAM" 2>/dev/null) || v=""
    echo "VPROBE $(date +%s) ${v:-FAIL}"
  fi
  if [ -n "$STREAM" ] && [ "$SIZE" -gt 2000000 ]; then
    OFF=$(( (RANDOM * 32768 + RANDOM) % (SIZE - 1048576) ))
    v=$(curl -s -o /dev/null -w '%{time_total}' --max-time TIMEOUT \
          -H "Range: bytes=$OFF-$((OFF + 1048575))" \
          "http://127.0.0.1:PORT$STREAM" 2>/dev/null) || v=""
    echo "VPROBE $(date +%s) ${v:-FAIL}"
  fi
done
'''


class NodeStream(threading.Thread):
    """One ssh session streaming both on-box probes and node vitals.

    One ssh per sample cost ~1.7 s, which both starved the sample rate and
    charged its own failures to the node. Everything on-box comes down a
    single session instead.
    """

    def __init__(self, node, duration, stop, port, timeout, data_path,
                 probe_user="", probe_pass="", media_path=""):
        super().__init__(daemon=True)
        self.name_ = "onbox"
        self.node = node
        self.duration = duration
        self.stop = stop
        self.port = port
        self.timeout = timeout
        self.rows = []
        self.samples = []
        self.data_samples = []
        self.data_path = data_path
        self.probe_user = probe_user
        self.probe_pass = probe_pass
        self.auth_ok = None
        self.viewer_ok = None
        self.media_path = media_path
        self.viewer_samples = []
        self.proc = None

    def run(self):
        script = (REMOTE_VITALS.replace("DURATION", str(self.duration + 2))
                  .replace("PORT", str(self.port))
                  .replace("TIMEOUT", str(self.timeout))
                  .replace("DATAPATH", self.data_path)
                  .replace("PROBEUSER", self.probe_user)
                  .replace("PROBEPASS", self.probe_pass)
                  .replace("MEDIAPATH", self.media_path))
        self.proc = subprocess.Popen(
            ["ssh", "-o", "ConnectTimeout=10", self.node, "bash -s"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True)
        self.proc.stdin.write(script)
        self.proc.stdin.close()
        for line in self.proc.stdout:
            f = line.split()
            if len(f) == 2 and f[0] == "AUTH":
                self.auth_ok = (f[1] == "ok")
                if not self.auth_ok:
                    print("  warning: probe login FAILED; data probe is "
                          "unauthenticated and will 403 on a gated route")
            if len(f) == 3 and f[0] == "PROBE":
                ms = None if f[2] == "FAIL" else float(f[2]) * 1000.0
                self.samples.append((int(f[1]), ms))
            if len(f) >= 2 and f[0] == "VIEWER":
                self.viewer_ok = (f[1] == "ok")
                print(f"  viewer probe: {'ready, 1 MiB ranged reads' if self.viewer_ok else 'FAILED to create a direct session'}")
            if len(f) == 3 and f[0] == "VPROBE":
                ms = None if f[2] == "FAIL" else float(f[2]) * 1000.0
                self.viewer_samples.append((int(f[1]), ms))
            if len(f) == 3 and f[0] == "DPROBE":
                ms = None if f[2] == "FAIL" else float(f[2]) * 1000.0
                self.data_samples.append((int(f[1]), ms))
            if len(f) == 7 and f[0] == "VITALS":
                self.rows.append({
                    "unix": int(f[1]), "load": float(f[2]),
                    "iowait_pct": int(f[3]), "idle_pct": int(f[4]),
                    "macha_read_mbs": int(f[5]), "macha_write_mbs": int(f[6]),
                })
            if self.stop.is_set():
                break


def generate_load(node, path, size_mb, stop):
    """Write through the FUSE mount: the real loader path, not a synthetic one."""
    cmd = (f"openssl enc -aes-256-ctr -pass pass:machaiopressure -nosalt "
           f"</dev/zero 2>/dev/null | "
           f"dd of={path} bs=1M count={size_mb} iflag=fullblock conv=fsync 2>&1 "
           f"| tail -1")
    p = subprocess.Popen(["ssh", "-o", "ConnectTimeout=10", node, cmd],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         text=True)
    out, _ = p.communicate()
    if not stop.is_set():
        print(f"  load generator finished: {out.strip()}")


HAPROXY_HARVEST = r"""
journalctl -u haproxy --since "@START" --no-pager 2>/dev/null |
  grep -E 'api/v1/(health|status)' |
  grep -oE '[0-9]+/[0-9]+/[0-9]+/[0-9-]+/[0-9]+ [0-9]{3}' |
  awk '{split($1,a,"/"); if (a[4] != "-") print a[4], $2}'
"""


def harvest_haproxy(node, start_unix):
    """haproxy's Tr field: time for macha to respond, excluding client network.

    This is the vantage the plan's acceptance criteria actually name. The
    external probe cannot be held to an absolute threshold because it carries
    an irreducible WAN round trip; this one carries none.
    """
    try:
        r = subprocess.run(
            ["ssh", "-o", "ConnectTimeout=10", node,
             HAPROXY_HARVEST.replace("@START", "@" + str(start_unix))],
            capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return None
    if r.returncode != 0:
        print(f"  warning: haproxy harvest failed rc={r.returncode} "
              f"{r.stderr.strip()[:120]}")
        return None
    samples, aborts = [], 0
    for line in r.stdout.split("\n"):
        f = line.split()
        if len(f) != 2:
            continue
        try:
            samples.append((0, float(f[0])))
        except ValueError:
            continue
    if not samples:
        return None
    out = summarise(samples)
    out["note"] = "haproxy Tr: server response time, excludes client network"
    return out


def count_client_aborts(node, start_unix):
    """CD-- terminations: the client gave up. The incident's signature."""
    try:
        r = subprocess.run(
            ["ssh", "-o", "ConnectTimeout=10", node,
             f'journalctl -u haproxy --since "@{start_unix}" --no-pager 2>/dev/null '
             f'| grep -c -- "CD--"'],
            capture_output=True, text=True, timeout=60)
        return int(r.stdout.strip() or 0)
    except Exception:
        return None


def verdict(report):
    """Stage 1 acceptance, applied to whichever vantage points were sampled."""
    lines, ok = [], True
    # Only vantages without a network floor are held to an absolute bound.
    # The external probe carries a WAN round trip no server fix can remove,
    # so it is reported against the idle baseline instead of a threshold.
    for name in ("onbox", "onbox-data", "viewer-read", "haproxy"):
        st = report["probes"].get(name)
        if not st or "p99_ms" not in st:
            continue
        p99, mx, failed = st["p99_ms"], st["max_ms"], st["failed"]
        good = p99 <= TARGET_P99_MS and mx <= TARGET_MAX_MS and failed == 0
        ok = ok and good
        lines.append(f"  {name:9s} p99 {p99:8.1f} ms (<= {TARGET_P99_MS:.0f})   "
                     f"max {mx:8.1f} ms (<= {TARGET_MAX_MS:.0f})   "
                     f"failed {failed}   {'PASS' if good else 'FAIL'}")
    ab = report.get("client_aborts")
    if ab is not None:
        good = ab == 0
        ok = ok and good
        lines.append(f"  {'aborts':9s} {ab} client CD-- termination(s) "
                     f"(== 0)                              {'PASS' if good else 'FAIL'}")
    ext = report["probes"].get("external")
    if ext and "p99_ms" in ext:
        lines.append(f"  {'external':9s} p99 {ext['p99_ms']:8.1f} ms  "
                     f"informational: includes WAN RTT, compare to idle baseline")
    if not any(n in report["probes"] for n in ("onbox", "haproxy")):
        ok = False
        lines.append("  no server-side vantage sampled; verdict not meaningful")
    return ok, lines


def run(args):
    stop = threading.Event()
    probes = []

    vitals = (NodeStream(args.node, args.duration, stop, args.port, args.timeout,
                         args.data_path, args.probe_user, args.probe_pass,
                         args.media_path)
              if args.node else None)
    if vitals:
        probes.append(vitals)
    if args.url:
        probes.append(ExternalProbe(args.url, args.interval, stop, args.timeout))

    print(f"sampling {args.duration}s  "
          f"({', '.join(p.name_ for p in probes)})"
          f"{'  + generating load' if args.generate_load else '  (observe only)'}")

    for p in probes:
        p.start()

    loader = None
    if args.generate_load:
        loader = threading.Thread(target=generate_load, daemon=True,
                                  args=(args.node, args.generate_load,
                                        args.load_mb, stop))
        loader.start()

    try:
        time.sleep(args.duration)
    except KeyboardInterrupt:
        print("\ninterrupted; reporting what was collected")
    stop.set()
    if vitals and vitals.proc:
        vitals.proc.terminate()
    for p in probes:
        p.join(timeout=args.timeout + 15)

    rows = vitals.rows[1:] if vitals and len(vitals.rows) > 1 else []
    started_unix = int(time.time() - args.duration)
    hap = harvest_haproxy(args.node, started_unix) if args.node else None
    aborts = count_client_aborts(args.node, started_unix) if args.node else None
    report = {
        "label": args.label,
        "started_unix": int(time.time() - args.duration),
        "duration_s": args.duration,
        "generated_load": bool(args.generate_load),
        "node": args.node,
        "url": args.url,
        "probes": {**{p.name_: summarise(p.samples) for p in probes},
                   **({"onbox-data": summarise(vitals.data_samples)}
                      if vitals and vitals.data_samples else {}),
                   **({"viewer-read": summarise(vitals.viewer_samples)}
                      if vitals and vitals.viewer_samples else {}),
                   **({"haproxy": hap} if hap else {})},
        "client_aborts": aborts,
        "external_reconnects": next((p.reconnects for p in probes
                                     if p.name_ == "external"), None),
        "raw": {p.name_: p.samples for p in probes} if args.raw else {},
        "vitals": {
            "samples": len(rows),
            "load_max": max((r["load"] for r in rows), default=None),
            "load_mean": round(statistics.fmean([r["load"] for r in rows]), 2) if rows else None,
            "iowait_pct_max": max((r["iowait_pct"] for r in rows), default=None),
            "iowait_pct_mean": round(statistics.fmean([r["iowait_pct"] for r in rows]), 1) if rows else None,
            "idle_pct_min": min((r["idle_pct"] for r in rows), default=None),
            "macha_write_mbs_max": max((r["macha_write_mbs"] for r in rows), default=None),
            "macha_read_mbs_max": max((r["macha_read_mbs"] for r in rows), default=None),
        },
        "rows": rows if args.raw else [],
    }

    print("\ncontrol-plane service time (GET /api/v1/health)")
    for name, s in report["probes"].items():
        if "p99_ms" in s:
            print(f"  {name:9s} n={s['count']:4d} failed={s['failed']:3d}  "
                  f"p50 {s['p50_ms']:7.1f}  p95 {s['p95_ms']:8.1f}  "
                  f"p99 {s['p99_ms']:8.1f}  max {s['max_ms']:8.1f} ms  "
                  f"(>250ms: {s['over_250ms']}, >1s: {s['over_1s']})")
        else:
            print(f"  {name:9s} n={s['count']:4d} failed={s['failed']:3d}  no successful samples")

    v = report["vitals"]
    if v["samples"]:
        print(f"\nnode        load max {v['load_max']:.2f} mean {v['load_mean']:.2f}   "
              f"iowait max {v['iowait_pct_max']}% mean {v['iowait_pct_mean']}%   "
              f"idle min {v['idle_pct_min']}%")
        print(f"macha io    write max {v['macha_write_mbs_max']} MB/s   "
              f"read max {v['macha_read_mbs_max']} MB/s")

    good, lines = verdict(report)
    print("\nstage 1 acceptance")
    for l in lines:
        print(l)
    print(f"  -> {'PASS' if good else 'FAIL'}")
    report["acceptance_pass"] = good

    if args.out:
        with open(args.out, "w") as f:
            json.dump(report, f, indent=2)
        print(f"\nwrote {args.out}")
    return 0 if good else 1


def compare(before_path, after_path):
    b = json.load(open(before_path))
    a = json.load(open(after_path))
    print(f"{'probe / metric':28s} {'before':>12s} {'after':>12s} {'change':>12s}")
    for name in ("onbox", "external"):
        sb, sa = b["probes"].get(name), a["probes"].get(name)
        if not sb or not sa or "p99_ms" not in sb or "p99_ms" not in sa:
            continue
        for k in ("p50_ms", "p95_ms", "p99_ms", "max_ms", "over_1s", "failed"):
            vb, va = sb.get(k), sa.get(k)
            if vb is None or va is None:
                continue
            if isinstance(vb, float) and vb:
                ch = f"{(va - vb) / vb * 100:+.0f}%"
            else:
                ch = f"{va - vb:+}"
            print(f"{name + '.' + k:28s} {vb:>12} {va:>12} {ch:>12}")
    for k in ("load_max", "iowait_pct_max", "macha_write_mbs_max"):
        vb, va = b["vitals"].get(k), a["vitals"].get(k)
        if vb is not None and va is not None:
            print(f"{'vitals.' + k:28s} {vb:>12} {va:>12} {'':>12}")
    print(f"\nacceptance  before={b.get('acceptance_pass')}  after={a.get('acceptance_pass')}")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--node", help="ssh target, e.g. root@10.34.1.50")
    p.add_argument("--url", help="base URL through the front end, e.g. https://host")
    p.add_argument("--port", type=int, default=7438, help="on-box API port")
    p.add_argument("--duration", type=int, default=120)
    p.add_argument("--interval", type=float, default=1.0)
    p.add_argument("--timeout", type=int, default=10)
    p.add_argument("--generate-load", metavar="PATH",
                   help="write through the FUSE mount at PATH to create loader I/O")
    p.add_argument("--load-mb", type=int, default=4096)
    p.add_argument("--data-path", default="/api/v1/health",
                   help="a DATA-lane path to probe on-box (default: health, "
                        "i.e. control only). A web asset isolates lane "
                        "starvation from disk contention.")
    p.add_argument("--probe-user", default="",
                   help="account for the DATA-lane probe; a gated route 403s "
                        "without it. Never commit a password into a script.")
    p.add_argument("--probe-pass", default="")
    p.add_argument("--media-path", default="",
                   help="namespace path of a large media file. The harness "
                        "mints its own direct-play session on it and issues a "
                        "1 MiB ranged read at a random offset each sample -- "
                        "the only probe that reliably reaches the disk, since "
                        "artwork and web assets stay in page cache.")
    p.add_argument("--label", default="")
    p.add_argument("--out", help="write the report as JSON")
    p.add_argument("--raw", action="store_true", help="include per-sample data")
    p.add_argument("--compare", nargs=2, metavar=("BEFORE", "AFTER"))
    args = p.parse_args()

    if args.compare:
        return compare(*args.compare)
    if not args.node and not args.url:
        p.error("need --node and/or --url (or --compare)")
    if args.generate_load and not args.node:
        p.error("--generate-load needs --node")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
