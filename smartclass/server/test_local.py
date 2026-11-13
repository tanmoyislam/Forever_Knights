"""Smoke test against a running server:  python test_local.py http://127.0.0.1:8000
Needs DEVICE_TOKENS="T02-D1:devtoken" ADMIN_TOKEN="admintoken" OFFLINE_AFTER_S=3 on the server."""
import json
import sys
import time
import urllib.error
import urllib.request

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8000"


def call(method, path, body=None, token="devtoken"):
    req = urllib.request.Request(BASE + path, method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())


seq0 = int(time.time())
tele = {"device_id": "T02-D1", "room_id": "R-305", "seq": seq0, "ts": int(time.time()), "uptime_s": 742,
        "temp_c": 29.0, "rh": 64, "heat_index_c": 31.2, "flame": False, "door": "LOCKED", "status": "SAFE",
        "heat_trend": "STABLE", "rssi": -61, "heap_kb": 182, "chip_temp_c": 41.5, "fw": "1.0.0", "quality": "good"}
event = {"device_id": "T02-D1", "room_id": "R-305", "seq": seq0 + 1, "ts": int(time.time()),
         "type": "CARD_TAP", "detail": "uid=04A1B2C3"}


def check(name, got, want):
    print(("PASS" if got == want else "FAIL"), name, "->", got)
    if got != want:
        sys.exit(1)


check("telemetry stored", call("POST", "/api/v1/telemetry", tele)[0], 201)
check("telemetry replay rejected", call("POST", "/api/v1/telemetry", tele)[0], 409)
check("event stored", call("POST", "/api/v1/events", event)[0], 201)
check("event replay rejected", call("POST", "/api/v1/events", event)[0], 409)
check("same seq on other endpoint rejected", call("POST", "/api/v1/events", dict(event, seq=seq0))[0], 409)
check("wrong token", call("POST", "/api/v1/telemetry", dict(tele, seq=seq0 + 2), token="nope")[0], 401)
check("unknown device", call("POST", "/api/v1/telemetry", dict(tele, device_id="X", seq=seq0 + 2))[0], 401)
check("bad field", call("POST", "/api/v1/telemetry", dict(tele, seq=seq0 + 2, door="OPEN"))[0], 422)
check("null temp accepted (sensor error)",
      call("POST", "/api/v1/telemetry", dict(tele, seq=seq0 + 3, temp_c=None, rh=None, heat_index_c=None,
                                             quality="bad"))[0], 201)
check("admin needs token", call("GET", "/api/v1/devices", token="devtoken")[0], 401)
code, devs = call("GET", "/api/v1/devices", token="admintoken")
check("device online", devs[0]["online"], True)
code, rows = call("GET", "/api/v1/telemetry?limit=500&device_id=T02-D1", token="admintoken")
check("stored exactly once", sum(1 for r in rows if r["seq"] == seq0), 1)
print("waiting for offline detection ...")
time.sleep(7)
check("device offline after silence", call("GET", "/api/v1/devices", token="admintoken")[1][0]["online"], False)
check("telemetry again", call("POST", "/api/v1/telemetry", dict(tele, seq=seq0 + 4))[0], 201)
check("device online again", call("GET", "/api/v1/devices", token="admintoken")[1][0]["online"], True)
code, logrows = call("GET", "/api/v1/state-log?limit=3", token="admintoken")
check("state log", [r["state"] for r in logrows], ["ONLINE", "OFFLINE", "ONLINE"])
print("ALL PASS")
