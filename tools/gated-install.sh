# Usage: sh gated-install.sh <version> <previous>. Installs only when no
# playback session can be live.
V=$1; PREV=$2
# A session lives 30 min (session_idle_ms) after its last request and ends
# silently, and a pipeline reclaim does not end it. So: nothing playback-related
# logged for 30 minutes. Blind spot: a client polling a paused session keeps
# it alive without a logged request; only the status API (view_status) sees it.
N=$(journalctl -u macha --since "-30min" --no-pager | grep -cE "playback\[|playback (stream|session|pipeline)")
echo "$(hostname) $(date -u +%H:%M:%SZ) playback lines in the last 30 min: $N"
if [ "$N" != "0" ]; then echo "NOT INSTALLING: playback within the session idle window"; exit 3; fi
cp /etc/macha/macha.yaml /etc/macha/macha.yaml.bak-$PREV && systemctl stop macha && tar xzf /tmp/macha-$V.tgz -C / && systemctl start macha || exit 1
until curl -s http://127.0.0.1:7438/api/v1/health | grep -q '"ok"'; do sleep 3; done
curl -s http://127.0.0.1:7438/api/v1/health; echo; date -u +%H:%M:%SZ
