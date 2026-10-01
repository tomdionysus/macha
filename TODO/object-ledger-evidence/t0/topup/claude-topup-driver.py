#!/usr/bin/env python3
# Top-up playback driver (object ledger T0, 2026-10-01; from the soak driver): runs ON a node, over loopback,
# with the on-box claude credentials. Per session: create, read the HLS
# playlists and a few segments, seek five times reading segments after each,
# delete. One JSON line per step with UTC time and elapsed ms.
import json, random, sys, time, urllib.error, urllib.parse, urllib.request

BASE = "http://127.0.0.1:7438"
MODES = (sys.argv[1] if len(sys.argv) > 1 else "remux,transcode").split(",")
SESSIONS = len(MODES)


def log(**fields):
    fields["utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    print(json.dumps(fields, sort_keys=True), flush=True)


def call(method, path, token, body=None, timeout=90):
    url = path if path.startswith("http") else BASE + path
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Authorization": "Bearer " + token}
    if data is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, method=method, data=data, headers=headers)
    started = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read(), (time.monotonic() - started) * 1000
    except urllib.error.HTTPError as e:
        return e.code, e.read(), (time.monotonic() - started) * 1000


def uris(playlist_bytes):
    return [l for l in playlist_bytes.decode(errors="replace").splitlines() if l and not l.startswith("#")]


def read_segments(token, stream_url, count):
    """Master playlist -> first variant -> the first `count` segments."""
    status, body, ms = call("GET", stream_url, token)
    if status != 200:
        return f"master {status}"
    variants = uris(body)
    media_url = urllib.parse.urljoin(stream_url, variants[0]) if variants and variants[0].endswith(".m3u8") else stream_url
    if media_url != stream_url:
        status, body, _ = call("GET", media_url, token)
        if status != 200:
            return f"media playlist {status}"
    segments = [s for s in uris(body)][:count]
    for segment in segments:
        status, data, seg_ms = call("GET", urllib.parse.urljoin(media_url, segment), token, timeout=120)
        if status != 200:
            return f"segment {status}"
    return f"ok segments={len(segments)}"


def main():
    user, password = open("/root/.macha-claude-credentials").read().split("\n")[:2]
    status, body, _ = call("POST", "/api/v1/session", "", {"credentials": {"username": user, "password": password}})
    token = json.loads(body)["token"]
    try:
        status, body, _ = call("GET", "/api/v1/catalogue/items?limit=500", token)
        items = [i for i in json.loads(body).get("items", [])
                 if i.get("kind") in ("movie", "episode") and i.get("media_ids")]
        random.shuffle(items)
        done = 0
        refused = 0
        for item in items:
            if done >= SESSIONS or refused >= 30:
                break
            media_id = item["media_ids"][0]
            request = {"media_id": media_id,
                       "preferences": {"mode": MODES[done]}}
            # The server does not choose: answer each choice it asks for with
            # the first value it offers (a container, a stream, ...).
            for _ in range(6):
                status, body, ms = call("POST", "/api/v1/playback/sessions", token, request)
                if status != 400:
                    break
                try:
                    error = json.loads(body).get("error", {})
                except Exception:
                    break
                if error.get("code") != "choice_required" or not error.get("choices"):
                    break
                request["preferences"][error["choice"]] = error["choices"][0]
            if status not in (200, 201):
                refused += 1
                code = ""
                try:
                    code = json.loads(body).get("status", "")
                except Exception:
                    pass
                log(step="create-refused", media_id=media_id, status=status, code=code, ms=round(ms))
                continue
            session = json.loads(body)
            sid = session.get("session_id")
            stream = (session.get("stream") or {}).get("url")
            log(step="create", media_id=media_id, title=item.get("title"), session=sid,
                mode=session.get("mode") or (session.get("plan") or {}).get("mode"), ms=round(ms))
            if stream:
                log(step="play", session=sid, result=read_segments(token, urllib.parse.urljoin(BASE, stream), 3))
            duration_ms = int(session.get("duration_ms") or 600_000)
            for fraction in (0.1, 0.5, 0.85, 0.3, 0.6):
                seek = int(duration_ms * fraction)
                status, body, ms = call("PATCH", f"/api/v1/playback/sessions/{sid}", token, {"seek_ms": seek})
                updated = json.loads(body) if status == 200 else {}
                stream = (updated.get("stream") or {}).get("url") or stream
                log(step="seek", session=sid, seek_ms=seek, status=status, ms=round(ms))
                if status == 200 and stream:
                    log(step="play", session=sid, result=read_segments(token, urllib.parse.urljoin(BASE, stream), 2))
            status, _, ms = call("DELETE", f"/api/v1/playback/sessions/{sid}", token)
            log(step="stop", session=sid, status=status, ms=round(ms))
            done += 1
        log(step="round-complete", sessions=done)
    finally:
        call("DELETE", "/api/v1/session", token)


main()
