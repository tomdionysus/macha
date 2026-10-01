#!/bin/sh
# T0 baseline top-up load on this node (object ledger experiment, 2026-10-01).
# Every CYCLE seconds until TOPUP_END: one remux and one transcode playback
# session (skipped while a real viewer is watching), then a 32 MiB file
# written through the FUSE mount and the previous one removed. Fills the
# hourly spread of K3 (claim.data_barrier_us) and K7 (playback.*).
TOPUP_END=$1
CYCLE=150
LOG=/root/claude-topup.log
dir=/mnt/machamedia/claude-topup/$(hostname)
exec >>"$LOG" 2>&1
echo "=== top-up start $(date -u +%Y-%m-%dT%H:%M:%SZ) until $(date -u -d @"$TOPUP_END" +%H:%M:%SZ)"

C=/root/.macha-claude-credentials
U=$(sed -n 1p $C); P=$(sed -n 2p $C); B=http://127.0.0.1:7438

while [ "$(date -u +%s)" -lt "$TOPUP_END" ]; do
    started=$(date +%s)
    echo "--- cycle $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    T=$(curl -s -m 30 -X POST $B/api/v1/session -H 'Content-Type: application/json' \
          -d "{\"credentials\":{\"username\":\"$U\",\"password\":\"$P\"}}" |
        python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])' 2>/dev/null)
    viewers=$(curl -s -m 30 -H "Authorization: Bearer $T" $B/api/v1/playback/status |
              python3 -c 'import sys,json;print(json.load(sys.stdin).get("sessions",0))' 2>/dev/null)
    curl -s -m 30 -o /dev/null -X DELETE -H "Authorization: Bearer $T" $B/api/v1/session
    if [ "$viewers" = "0" ]; then
        timeout 600 python3 /root/claude-topup-driver.py remux,transcode | grep -v create-refused
    else
        echo "playback skipped: viewers=${viewers:-unreachable}"
    fi

    mkdir -p "$dir" 2>/dev/null
    new="$dir/load-$(date -u +%Y%m%dT%H%M%SZ).bin"
    if timeout 300 dd if=/dev/urandom of="$new" bs=4M count=8 conv=fsync status=none; then
        echo "wrote $new"
        for old in "$dir"/load-*.bin; do
            [ "$old" = "$new" ] && continue
            rm -f "$old" && echo "removed $old"
        done
    else
        echo "write failed: $new"
    fi

    left=$(( CYCLE - ($(date +%s) - started) ))
    [ "$left" -gt 0 ] && sleep "$left"
done
rm -rf "$dir"
echo "=== top-up end $(date -u +%Y-%m-%dT%H:%M:%SZ)"
