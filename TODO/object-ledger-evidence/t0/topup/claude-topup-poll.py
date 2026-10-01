#!/usr/bin/env python3
# T0 baseline top-up API poller (object ledger experiment, 2026-10-01): until
# the end time, every 30 s, the K6 routes over loopback with the on-box
# claude credentials. Logs in once and again only on 401.
import json, random, sys, time, urllib.error, urllib.request

BASE = "http://127.0.0.1:7438"
END = int(sys.argv[1])
ROUTES = ["/api/v1/status", "/api/v1/torrents/jobs", "/api/v1/catalogue/status",
          "/api/v1/catalogue/items?limit=50", "/api/v1/catalogue/search?q=the&limit=20"]


def call(method, path, token="", body=None):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Authorization": "Bearer " + token} if token else {}
    if data is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(BASE + path, method=method, data=data, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:
        return 0, str(e).encode()


def login():
    user, password = open("/root/.macha-claude-credentials").read().split("\n")[:2]
    status, body = call("POST", "/api/v1/session", body={"credentials": {"username": user, "password": password}})
    return json.loads(body)["token"] if status in (200, 201) else ""


token = ""
item_ids = []
failures = 0
while time.time() < END:
    started = time.time()
    if not token:
        token = login()
    for route in ROUTES + ([f"/api/v1/catalogue/items/{random.choice(item_ids)}"] if item_ids else []):
        status, body = call("GET", route, token)
        if status == 401:
            token = ""
            break
        if status != 200:
            failures += 1
            print(time.strftime("%H:%M:%SZ", time.gmtime()), route, status, body[:120], flush=True)
        elif route.startswith("/api/v1/catalogue/items?"):
            try:
                item_ids = [i["id"] for i in json.loads(body).get("items", []) if "id" in i] or item_ids
            except Exception:
                pass
    time.sleep(max(0.0, 30 - (time.time() - started)))
if token:
    call("DELETE", "/api/v1/session", token)
print("poller end, non-200 responses:", failures, flush=True)
