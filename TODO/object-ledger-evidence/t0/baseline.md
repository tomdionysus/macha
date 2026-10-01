# T0 baseline: 0.74.0 on gbni-1 and fi-1

The kill criteria's baseline and variance (spec, "The kill criteria,
quantified"), from 2026-09-30 07:04Z to 2026-10-01 20:43Z: the 31-hour
soak and the six-hour top-up that followed it, one window a minute per
node. fi-1: 2,261 windows (1,888 idle, 373 loaded); gbni-1: 2,254 (1,779
idle, 475 loaded).

- Logs: `observations-{fi-1,gbni-1}.jsonl.gz`; full reports
  `report-{fi-1,gbni-1}.txt` from `observation_report.py`.
- Load: `soak/` (every 4 hours: driven playback, 1 GiB through FUSE);
  `topup/` (every 150 s or as long as a cycle takes: a remux and a
  transcode session, 32 MiB through FUSE; the K6 routes polled every 30 s;
  three restarts per node, spaced for viewers). Real viewers were present
  throughout; on fi-1 they held a session in 47 of 84 top-up cycles, which
  skipped the driven playback.

Figures are microseconds unless named `_ns`, bucket upper bounds (within
12.5%). "Hourly" is min/median/max of the per-hour p50 and p99 over hours
with at least 20 values: the spread the threshold rule takes. **Thin**
marks a series with fewer than five such hours: its threshold falls back to
the whole-run figures and is weak.

## K1. Claim walk cost

`maintenance.claim_walk.examine_us`

| node | n | p50 | p99 | idle p50/p99 | loaded p50/p99 | hourly p50 | hourly p99 |
|---|---|---|---|---|---|---|---|
| fi-1 | 480,148 | 2 | 19 | 2/19 | 3/35 | 2/2/5 | 17/19/47 |
| gbni-1 | 386,136 | 3 | 31 | 3/23 | 4/79 | 2/3/4 | 21/24/127 |

In-suite `claim_walk.per_object`: see README (bench-fi-1.txt).

## K2. GC sweep cost

| node | series | n | p50 | p99 | idle p50/p99 | loaded p50/p99 | hourly p50 | hourly p99 |
|---|---|---|---|---|---|---|---|---|
| fi-1 | `gc.per_object_ns` | 1,266 | 6,143 | 163,839 | 6,143/180,223 | 4,607/147,455 | 1,919/6,655/18,431 | 2,765/20,479/6,416,968 |
| fi-1 | `gc.step_us` | 1,380 | 383 | 10,239 | 383/10,239 | 191/9,215 | 6/415/1,151 | 10/1,215/410,686 |
| gbni-1 | `gc.per_object_ns` | 568 | 327,679 | 1,703,935 | 13,311/1,441,791 | 655,359/3,407,871 | 3,071/393,215/851,967 | 5,625/851,967/4,718,591 |
| gbni-1 | `gc.step_us` | 603 | 15,359 | 106,495 | 831/90,111 | 36,863/114,687 | 207/24,575/53,247 | 360/53,247/203,031 |

## K3. Quantum-commit claim latency

`claim.data_barrier_us` (loaded windows only, by definition)

| node | n | p50 | p99 | hourly p50 | hourly p99 |
|---|---|---|---|---|---|
| fi-1 | 117 | 4,607 | 840,588 | 4,607/4,607/4,607 (thin: 2 hours) | 4,633/4,876/5,119 |
| gbni-1 | 713 | 360,447 | 2,097,151 | 196,607/409,599/491,519 | 1,633,121/1,869,328/4,533,580 |

## K4. Repair throughput

Per minute over the covered time; cumulative gauges summed across restarts.

| node | bytes (gauge) | push examined | pull examined | passes completed |
|---|---|---|---|---|
| fi-1 | 21.1 MB/min (47.7 GB) | 9.76/min | 17.60/min | 0 |
| gbni-1 | 18.8 MB/min (42.3 GB) | 5.00/min | 0.06/min | 0 |

`maintenance.repair.bytes.{idle,loaded}` and the step histograms are in
the reports. No pass completed: a pass must settle every local object
(push) and every live object (pull) at these rates, one to three months for
~650-830k objects (ACTIVE, T0's baseline).

## K5. Release and GC rates

| node | released data | released control | pruned data | pruned control |
|---|---|---|---|---|
| fi-1 | 1.15/min | 1.96/min | 0 | 0.12/min |
| gbni-1 | 1.09/min | 5.75/min | 0.19/min | 0.54/min |

`maintenance.gc.reclaimed_bytes`, `maintenance.tombstones.collected` and
`catalogue.control_gc.removed`: in the reports.

## K6. API latency

p50/p99 overall; hourly spread of p50 and p99.

| node | route | n | p50 | p99 | hourly p50 | hourly p99 |
|---|---|---|---|---|---|---|
| fi-1 | `GET /api/v1/status` | 8,642 | 639 | 2,559 | 575/639/703 | 767/1,343/23,022 |
| fi-1 | `GET /api/v1/torrents/jobs` | 2,160 | 831 | 5,119 | 351/831/1,023 | 1,246/4,607/8,191 |
| fi-1 | `GET /api/v1/catalogue/items` | 976 | 212,991 | 1,441,791 | 196,607/212,991/245,759 | 360,447/720,895/35,806,215 |
| fi-1 | `GET /api/v1/catalogue/items/:id` | 849 | 103 | 1,151 | 95/103/119 | 511/1,023/10,239 |
| fi-1 | `GET /api/v1/catalogue/search` | 770 | 24,575 | 90,111 | 22,527/24,575/26,623 | 32,767/81,919/5,767,167 |
| fi-1 | `GET /api/v1/catalogue/status` | 740 | 12,287 | 45,055 | 12,287/12,287/13,311 | 18,431/30,719/65,089 |
| gbni-1 | `GET /api/v1/status` | 1,023 | 959 | 10,239 | 895/959/959 | 1,663/8,191/20,479 |
| gbni-1 | `GET /api/v1/torrents/jobs` | 807 | 7,679 | 81,919 | 1,535/9,215/20,479 | 7,679/36,863/146,145 |
| gbni-1 | `GET /api/v1/catalogue/items` | 955 | 524,287 | 2,097,151 | 1,663/557,055/655,359 | 147,455/1,966,079/33,554,431 |
| gbni-1 | `GET /api/v1/catalogue/items/:id` | 848 | 191 | 4,607 | 159/175/207 | 1,341/4,095/5,631 |
| gbni-1 | `GET /api/v1/catalogue/search` | 758 | 53,247 | 163,839 | 49,151/53,247/65,535 | 144,035/163,839/229,375 |
| gbni-1 | `GET /api/v1/catalogue/status` | 762 | 32,767 | 6,815,743 | 71/30,719/45,055 | 1,572,863/3,407,871/8,057,129 |

## K7. Playback

| node | series | n | p50 | p99 | hourly p50 | hourly p99 |
|---|---|---|---|---|---|---|
| fi-1 | `create_us` | 187 | 7,340,031 | 23,975,799 | thin: 3 hours | |
| fi-1 | `first_fragment_us` | 604 | 9,437,183 | 14,979,286 | 5,767,167/9,437,183/10,485,759 | 14,501,802/14,902,155/14,979,286 |
| fi-1 | `start_ready_us` | 5 | 2,883,583 | 14,409,787 | thin: none | |
| fi-1 | `update_ready_us` | 9 | 5,767,167 | 29,206,045 | thin: none | |
| gbni-1 | `create_us` | 323 | 393,215 | 4,718,591 | 639/507,903/1,179,647 | 3,670,015/3,956,923/5,610,545 |
| gbni-1 | `first_fragment_us` | 1,555 | 524,287 | 14,680,063 | 425,983/458,751/983,039 | 11,534,335/14,434,371/14,680,063 |
| gbni-1 | `start_ready_us` | 8 | 3,670,015 | 6,270,965 | thin: none | |
| gbni-1 | `update_ready_us` | 6 | 6,815,743 | 10,196,577 | thin: none | |

`start_ready_us` and `update_ready_us` record only the asynchronous start
and the pipeline-restarting seek; the driver's sessions rarely took either
path (most seeks take the fast path: `playback.seek_fastpath.taken`). They
stay thin whatever the load, and a threshold on them is weak. fi-1's
playback errors (seek 503 after ~15 s, segment 500, `extent unavailable`)
are part of this baseline.

## K8. FUSE publication

`fuse_publication_bytes_committed`, summed across restarts: gbni-1 5.26
MB/min over the run (11.8 GB), fi-1 4.95 MB/min (11.2 GB). The top-up
wrote 32 MiB per cycle on each node.

## K9. Resident memory

| node | min | median | max |
|---|---|---|---|
| fi-1 | 384 MB | 1,118 MB | 2,067 MB |
| gbni-1 | 559 MB | 1,044 MB | 1,920 MB |

## K10. Startup, recovery, shutdown

Milliseconds. Each row is one restart: the stopping process's `shutdown`
event (its `elapsed_ms`), then the new process's `backend_online`,
`services_ready` and `cluster_stable`.

| node | restart (UTC) | cause | shutdown | backend online | services ready | cluster stable |
|---|---|---|---|---|---|---|
| fi-1 | 09-30 07:04 | first start of 0.74.0 | | 2,737 | 12,527 | 32,958 |
| fi-1 | 09-30 10:03 | after a USB I/O fault | 610 | 46,119 | 15,420 | 30,001 |
| fi-1 | 09-30 13:20 | after a USB I/O fault | 1,362 | 45,984 | 11,732 | 24,673 |
| fi-1 | 10-01 16:00 | unattributed (below) | 8,214 | 1,835 | 13,053 | 62,462 |
| fi-1 | 10-01 16:22 | top-up 1 | 7,474 | 744 | 12,923 | 61,000 |
| fi-1 | 10-01 17:15 | top-up 2 | 4,966 | 1,149 | 12,907 | 56,535 |
| fi-1 | 10-01 17:26 | top-up 3 | killed at 60 s | 364 | 11,118 | 123,101 |
| gbni-1 | 09-30 07:12 | first start of 0.74.0 | | 5,808 | 43,187 | 62,324 |
| gbni-1 | 10-01 15:25 | top-up 1 | 2,172 | 5,926 | 43,041 | 68,328 |
| gbni-1 | 10-01 15:35 | top-up 2 | 5,894 | 5,785 | 40,104 | 98,368 |
| gbni-1 | 10-01 15:46 | top-up 3 | killed at 60 s | 5,781 | 40,498 | 131,046 |

The 16:00:01Z restart of fi-1 was not issued by the top-up's restart loop
(its log has none; fi-1 was held by a viewer). The journal shows an SSH
session as root from the laptop's address (10.35.1.132) at 16:00:00Z, one
second before the stop. Unattributed; a clean restart, counted as one.

**Two of six top-up restarts hung and were killed** (fi-1 17:26Z, gbni-1
15:46Z), each with a FUSE publication in flight: `Service::stop` began, the
FUSE main loop did not return, the FUSE watchdog declared the mount gone
during shutdown, and on gbni-1 an async data publication retried a
cancelled write with back-off (250 ms to 16 s) until systemd's 60 s
SIGKILL. Both nodes recovered alone (fi-1 replayed 8 durable FUSE
operations; the file being written survived at full size). Not yet
reproduced on purpose or explained (ACTIVE). For the threshold rule, the
baseline's slowest clean shutdown is 8.2 s (fi-1) and 5.9 s (gbni-1); a
killed shutdown is its own finding, not a time.

Backend recovery (`backend_online` after `backend_offline`): fi-1's two
USB faults only, 46.1 s and 46.0 s. gbni-1 had none.

## K11. In-suite

See README (suites, `bench-fi-1.txt`, coverage); five runs per figure.
