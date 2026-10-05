import re,sys
def load(p):
    h={};c={};g={};ev=[]
    sec=None
    for line in open(p):
        line=line.rstrip("\n")
        if line.startswith("histograms"): sec="h"; continue
        if line.startswith("counters"): sec="c"; continue
        if line.startswith("gauges"): sec="g"; continue
        if line.startswith("events"): sec="e"; continue
        if not line.startswith("  "): continue
        s=line.strip()
        if sec=="h":
            m=re.match(r"(.+?): n=(\d+) p50=(\d+) p90=(\d+) p99=(\d+) max=(\d+)",s)
            if m: h[m.group(1)]=tuple(int(x) for x in m.groups()[1:])
        elif sec in("c","g"):
            m=re.match(r"(.+?): (\d+) \(([\d.]+)/min",s)
            if m: (c if sec=="c" else g)[m.group(1)]=(int(m.group(2)),float(m.group(3)))
            m=re.match(r"(rss_bytes): min=(\d+) median=(\d+) max=(\d+)",s)
            if m: g["rss"]=(int(m.group(3)),int(m.group(4)))
        elif sec=="e": ev.append(s)
    return h,c,g,ev
H=["claim.data_barrier_us","maintenance.claim_walk.examine_us","maintenance.gc.per_object_ns","maintenance.gc.step_us","maintenance.inventory.build_us","api GET /api/v1/status","api GET /api/v1/health","api GET /api/v1/torrents/jobs","api GET /api/v1/catalogue/items","api GET /api/v1/catalogue/items/:id","api GET /api/v1/catalogue/search","api GET /api/v1/catalogue/status","playback.create_us","playback.first_fragment_us","api PATCH /api/v1/playback/sessions/:id","maintenance.repair.step_us.loaded"]
G=["fuse_publication_bytes_committed","repair_bytes_transferred","repair_push_examined","repair_pull_examined","repair_pull_unsourceable","repair_passes_completed","maintenance_wakeups"]
C=["claim.data_barrier.ids","retention.released.data","retention.released.control","maintenance.gc.examined"]
for node in ("fi-1","gbni-1"):
    a=load(f"build/k81/t0-topup-{node}.txt"); b=load(f"build/k81/report-{node}.txt")
    print(f"=== {node}: T0 top-up (0.74.0) -> today (0.81.0)")
    for k in H:
        x=a[0].get(k); y=b[0].get(k)
        f=lambda v: "-" if not v else f"n={v[0]} p50={v[1]} p99={v[3]}"
        print(f"  {k:44s} {f(x):38s} -> {f(y)}")
    for k in C:
        x=a[1].get(k); y=b[1].get(k)
        f=lambda v: "-" if not v else f"{v[1]}/min"
        print(f"  {k:44s} {f(x):38s} -> {f(y)}")
    for k in G:
        x=a[2].get(k); y=b[2].get(k)
        f=lambda v: "-" if not v else f"{v[1]}/min (total {v[0]})"
        print(f"  {k:44s} {f(x):38s} -> {f(y)}")
    print(f"  rss median/max MB  {[v//(1<<20) for v in a[2].get('rss',(0,0))]} -> {[v//(1<<20) for v in b[2].get('rss',(0,0))]}")
    for tag,r in (("T0",a),("now",b)):
        print(f"  events {tag}: "+" | ".join(re.sub(r'^\d+ ','',e) for e in r[3] if e.split()[1] in ("shutdown","services_ready","cluster_stable","start")))
