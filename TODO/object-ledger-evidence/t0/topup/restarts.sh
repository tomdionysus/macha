#!/bin/sh
# K10 top-up: three restarts per node, at least 10 minutes between any two.
# A node is restarted only between top-up driver runs and with no viewer
# playing; when one node is held, the other goes first.
cd /Users/tom/devroot/macha/build || exit 1
FI=10.35.1.10; GB=10.44.1.50
fi_left=3; gb_left=3; last=$GB

free() {
    ssh -o ConnectTimeout=15 root@$1 'pgrep -f [c]laude-topup-driver >/dev/null' && return 1
    viewers=$(ssh -o ConnectTimeout=15 root@$1 'bash -s' < claude-viewers.sh | sed -n 's/.* sessions \([0-9]*\) .*/\1/p')
    echo "$(date -u +%H:%M:%SZ) $1 viewers=${viewers:-?}"
    [ "$viewers" = "0" ]
}

restart() {
    ssh -o ConnectTimeout=15 root@$1 'systemctl restart macha; echo restart_rc=$?' | sed "s/^/$(date -u +%H:%M:%SZ) $1 /"
    last=$1
    sleep 600
}

while [ $((fi_left + gb_left)) -gt 0 ]; do
    # Prefer the node not restarted last.
    if [ "$last" = "$GB" ]; then first=$FI; second=$GB; else first=$GB; second=$FI; fi
    done_one=
    for host in $first $second; do
        if [ "$host" = "$FI" ]; then left=$fi_left; else left=$gb_left; fi
        [ "$left" -gt 0 ] || continue
        if free $host; then
            restart $host
            if [ "$host" = "$FI" ]; then fi_left=$((fi_left - 1)); else gb_left=$((gb_left - 1)); fi
            done_one=1
            break
        fi
    done
    [ -n "$done_one" ] || sleep 30
done
echo "$(date -u +%H:%M:%SZ) restarts done"
