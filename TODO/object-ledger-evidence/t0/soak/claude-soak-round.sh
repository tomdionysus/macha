#!/bin/sh
# One T0 soak load round on this node (object ledger experiment, 2026-09-30).
# Driven playback (skipped while a real viewer is watching), then 1 GiB
# written through the FUSE mount and the previous round's file removed.
# Does nothing after SOAK_END (UTC epoch seconds); remove its cron entry
# once the soak is over.
SOAK_END=1790864400   # 2026-10-01 14:20Z
LOG=/root/claude-soak.log
[ "$(date -u +%s)" -ge "$SOAK_END" ] && exit 0
exec >>"$LOG" 2>&1
echo "=== round $(date -u +%Y-%m-%dT%H:%M:%SZ) $(hostname)"

C=/root/.macha-claude-credentials
U=$(sed -n 1p $C); P=$(sed -n 2p $C); B=http://127.0.0.1:7438
T=$(curl -s -X POST $B/api/v1/session -H 'Content-Type: application/json' \
      -d "{\"credentials\":{\"username\":\"$U\",\"password\":\"$P\"}}" |
    python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])')
viewers=$(curl -s -H "Authorization: Bearer $T" $B/api/v1/playback/status |
          python3 -c 'import sys,json;print(json.load(sys.stdin).get("sessions",0))')
curl -s -o /dev/null -X DELETE -H "Authorization: Bearer $T" $B/api/v1/session
if [ "$viewers" = "0" ]; then
    python3 /root/claude-playback-driver.py 3 | grep -v create-refused
else
    echo "playback skipped: $viewers session(s) already playing"
fi

dir=/mnt/machamedia/claude-soak
mkdir -p "$dir"
new="$dir/load-$(date -u +%Y%m%dT%H%M%SZ).bin"
started=$(date +%s)
dd if=/dev/urandom of="$new" bs=4M count=256 conv=fsync status=none &&
    echo "wrote $new in $(( $(date +%s) - started )) s"
for old in "$dir"/load-*.bin; do
    [ "$old" = "$new" ] && continue
    rm -f "$old" && echo "removed $old"
done
