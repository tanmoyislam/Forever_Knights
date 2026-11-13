# SmartClass — FastRobox 1.0 (Levels 1–2)

One ESP32-S3-CAM classroom safety device plus a cloud platform that stores every reading exactly once.

```
 DHT11 ─┐                                   HTTPS (TLS, certificate verified)
 Flame ─┤  ESP32-S3-CAM  ── Wi-Fi ──────────────────────────────►  Platform (server/main.py)
 RC522 ─┤  6 FreeRTOS tasks, 2 cores        POST /api/v1/telemetry        │
 BOOT  ─┘  NVS settings · LittleFS pages    POST /api/v1/events           ▼
            │                                                       PostgreSQL
            └─ local status page  http://smartclass-<teamid>.local   (telemetry, events, devices,
               setup page         http://192.168.4.1 (AP mode)        seq_ledger, rejected, state log)
```
AI services, weather service and the dashboard join this diagram from Level 4.

## Wiring

| Part | Pin | ESP32-S3 GPIO |
|---|---|---|
| DHT11 | DATA | 1 |
| Flame sensor | DO (active LOW) | 2 |
| Buzzer | + | 39 |
| RC522 | SDA/SS, RST, SCK, MOSI, MISO | 21, 40, 47, 41, 42 |
| RGB LED | onboard | 48 |
| BOOT button | onboard | 0 |

RC522 and DHT11 are powered from 3V3.

## Build and flash the firmware

1. Arduino IDE 2.x, Boards Manager: **esp32 by Espressif Systems 3.x**.
2. Library Manager: **MFRC522** (miguelbalboa), **DHT sensor library** (Adafruit) and **Adafruit Unified Sensor**.
3. Open `firmware/smartclass/smartclass.ino`. Set `TEAM_ID` (and optionally `DEFAULT_SERVER_URL`) at the top.
   These are not secrets. No password, key or token is in the code.
4. Tools: Board **ESP32S3 Dev Module**, USB CDC On Boot **Enabled**, PSRAM **OPI PSRAM**,
   Partition Scheme: any with a SPIFFS/LittleFS partition (for example *8M with spiffs*).
5. Upload, open Serial Monitor at 115200 baud, line ending *Newline*.

The two web pages are written into LittleFS by the firmware at start-up (from `web_pages.h`) and served from LittleFS.

## Run the platform

Environment variables (set in the host's dashboard, never in the repository):

| Variable | Meaning |
|---|---|
| `DATABASE_URL` | PostgreSQL connection string |
| `DEVICE_TOKENS` | `device_id:token` pairs, comma separated, e.g. `T02-D1:<long random string>` |
| `ADMIN_TOKEN` | token for the read-only GET endpoints |

Generate tokens with `python -c "import secrets; print(secrets.token_urlsafe(32))"`.

**Cloud (public HTTPS):** create a Python web service from the `server/` folder on any host that gives an HTTPS
address and PostgreSQL (`render.yaml` is a ready blueprint for Render). Build `pip install -r requirements.txt`,
start `uvicorn main:app --host 0.0.0.0 --port $PORT`.

**Local test (SQLite, no TLS):**
```
cd server
pip install -r requirements.txt
DEVICE_TOKENS="T02-D1:devtoken" ADMIN_TOKEN="admintoken" OFFLINE_AFTER_S=3 uvicorn main:app --port 8000
python test_local.py http://127.0.0.1:8000
```

## First set-up of a device

1. With no saved network the device starts the access point `SmartClass-<TeamID>`; the light blinks blue and the
   Serial Monitor prints `[NET] setup mode: AP=... password=...` (8 random characters, generated on first boot, kept in NVS).
2. Join the access point, open `http://192.168.4.1`, pick the network, enter its password, the **Room ID**,
   the **device token** and the server URL. Everything is saved in NVS.
3. On success the Serial Monitor prints the IP address and the mDNS name; the status page is at
   `http://<ip>` and `http://smartclass-<teamid>.local`.
4. Wrong password or unreachable network: the device is back in setup mode after 12 s and the setup page shows the error.
5. Hold **BOOT for 5 s**, or type `reset`, to clear the saved details and restart in setup mode.

## How it behaves

- **Reconnection.** Wi-Fi loss never restarts the board. It retries with gaps of 1, 2, 4, 8, then 10 s. After a power cut
  it reconnects from NVS with no setup. Server loss is retried with gaps of 1, 2, 4, then 8 s.
- **Exactly once.** Each message carries `seq`, which only increases: blocks of 50 numbers are reserved in NVS before use,
  so a restart can skip numbers but never reuse one. A message is re-sent with the same `seq` until the server answers.
  The server inserts `(device_id, seq)` into `seq_ledger` (primary key); a second arrival gets **HTTP 409**, is logged
  in `rejected`, and is not stored again.
- **Time.** `ts` is NTP time taken when the reading was made, not when it was sent; readings queue (40 deep) while offline.
- **Online/offline.** `devices.online` goes false after 15 s of silence (checked every 2 s) and true on the next message;
  each change is written to `device_state_log`.
- **TLS.** HTTPS only; the server certificate is checked against the ESP-IDF root CA bundle.

## Serial commands

`help`, `status`, `health`, `tasks`, `ver`, `hang`, `reset`, `replay` (re-sends the last delivered message to show the 409).

## Looking at the database

```
curl -H "Authorization: Bearer $ADMIN_TOKEN" https://<host>/api/v1/devices
curl -H "Authorization: Bearer $ADMIN_TOKEN" "https://<host>/api/v1/telemetry?limit=10"
curl -H "Authorization: Bearer $ADMIN_TOKEN" "https://<host>/api/v1/events?limit=10"
curl -H "Authorization: Bearer $ADMIN_TOKEN" "https://<host>/api/v1/rejected?limit=10"
curl -H "Authorization: Bearer $ADMIN_TOKEN" "https://<host>/api/v1/state-log?limit=10"
```
Or in SQL: `SELECT seq, to_timestamp(ts), temp_c, rh, status FROM telemetry ORDER BY id DESC LIMIT 10;`
