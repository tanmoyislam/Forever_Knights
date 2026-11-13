"""SmartClass platform - Level 2: secure telemetry + events ingest.

- HTTPS is terminated by the host (Render / Railway / Fly ...); the device verifies its certificate.
- Every message is stored exactly once: (device_id, seq) is unique, a replay gets HTTP 409.
- A device is marked offline after OFFLINE_AFTER_S seconds of silence, online again when it returns.
- No secret is in this file: tokens and the database URL come from environment variables.

Environment:
  DATABASE_URL   postgres://...  (omit for a local SQLite file, dev only)
  DEVICE_TOKENS  "T02-D1:<token>[,T02-D2:<token>]"   device_id:token pairs
  ADMIN_TOKEN    token for the read-only /api/v1/* GET endpoints
"""
import asyncio
import contextlib
import hmac
import json
import logging
import os
import queue
import sqlite3
import time
from typing import Literal, Optional

from pydantic import BaseModel, Field, ValidationError
from starlette.applications import Starlette
from starlette.concurrency import run_in_threadpool
from starlette.requests import Request
from starlette.responses import JSONResponse
from starlette.routing import Route

log = logging.getLogger("smartclass")
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")

DATABASE_URL = os.environ.get("DATABASE_URL", "sqlite:///smartclass.db")
ADMIN_TOKEN = os.environ.get("ADMIN_TOKEN", "")
OFFLINE_AFTER_S = int(os.environ.get("OFFLINE_AFTER_S", "15"))
MAX_BODY = 4096

DEVICE_TOKENS = {}
for pair in os.environ.get("DEVICE_TOKENS", "").split(","):
    if ":" in pair:
        dev, tok = pair.split(":", 1)
        if dev.strip() and tok.strip():
            DEVICE_TOKENS[dev.strip()] = tok.strip()


# ----------------------------------------------------------------- database
class Tx:
    def __init__(self, conn, pg):
        self.conn, self.pg = conn, pg

    def q(self, sql, params=()):
        if not self.pg:
            sql = sql.replace("%s", "?")
        return self.conn.execute(sql, params)

    def rows(self, sql, params=()):
        cur = self.q(sql, params)
        cols = [d[0] for d in cur.description]
        return [dict(zip(cols, r)) for r in cur.fetchall()]


class DB:
    """Tiny connection pool that works for PostgreSQL (production) and SQLite (local dev)."""

    def __init__(self, url):
        self.url = url
        self.pg = url.startswith(("postgres://", "postgresql://"))
        self.pool = queue.LifoQueue()

    def _connect(self):
        if self.pg:
            import psycopg
            return psycopg.connect(self.url)
        conn = sqlite3.connect(self.url.replace("sqlite:///", ""), check_same_thread=False, timeout=10)
        conn.execute("PRAGMA journal_mode=WAL")
        return conn

    @contextlib.contextmanager
    def tx(self):
        try:
            conn = self.pool.get_nowait()
        except queue.Empty:
            conn = self._connect()
        try:
            yield Tx(conn, self.pg)
            conn.commit()
        except Exception:
            with contextlib.suppress(Exception):
                conn.close()
            raise
        else:
            self.pool.put(conn)

    def run(self, fn):
        """Run fn(tx) in one transaction; retry once if a pooled connection had gone stale."""
        for attempt in (1, 2):
            try:
                with self.tx() as t:
                    return fn(t)
            except Exception:
                if attempt == 2:
                    raise
                log.exception("db error, retrying with a fresh connection")


db = DB(DATABASE_URL)

SCHEMA = [
    """CREATE TABLE IF NOT EXISTS seq_ledger (
        device_id TEXT NOT NULL, seq BIGINT NOT NULL, kind TEXT NOT NULL, received_at BIGINT NOT NULL,
        PRIMARY KEY (device_id, seq))""",
    """CREATE TABLE IF NOT EXISTS telemetry (
        id {PK}, device_id TEXT NOT NULL, room_id TEXT NOT NULL, seq BIGINT NOT NULL, ts BIGINT NOT NULL,
        uptime_s BIGINT, temp_c REAL, rh REAL, heat_index_c REAL, flame BOOLEAN, door TEXT, status TEXT,
        heat_trend TEXT, rssi INTEGER, heap_kb INTEGER, chip_temp_c REAL, fw TEXT, quality TEXT,
        received_at BIGINT NOT NULL, UNIQUE (device_id, seq))""",
    """CREATE TABLE IF NOT EXISTS events (
        id {PK}, device_id TEXT NOT NULL, room_id TEXT NOT NULL, seq BIGINT NOT NULL, ts BIGINT NOT NULL,
        type TEXT NOT NULL, detail TEXT NOT NULL, received_at BIGINT NOT NULL, UNIQUE (device_id, seq))""",
    """CREATE TABLE IF NOT EXISTS devices (
        device_id TEXT PRIMARY KEY, room_id TEXT, online BOOLEAN NOT NULL, last_seen BIGINT NOT NULL,
        last_seq BIGINT, fw TEXT)""",
    """CREATE TABLE IF NOT EXISTS device_state_log (
        id {PK}, device_id TEXT NOT NULL, state TEXT NOT NULL, at BIGINT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS rejected (
        id {PK}, device_id TEXT NOT NULL, seq BIGINT NOT NULL, kind TEXT NOT NULL, reason TEXT NOT NULL,
        at BIGINT NOT NULL)""",
]


def init_db():
    pk = "BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY" if db.pg else "INTEGER PRIMARY KEY AUTOINCREMENT"

    def go(t):
        for stmt in SCHEMA:
            t.q(stmt.replace("{PK}", pk))

    db.run(go)


# ------------------------------------------------------------------- models
class Base(BaseModel):
    device_id: str = Field(min_length=1, max_length=32)
    room_id: str = Field(min_length=1, max_length=32)
    seq: int = Field(ge=0, le=2**53)
    ts: int = Field(ge=1_600_000_000, le=4_102_444_800)


class Telemetry(Base):
    uptime_s: int = Field(ge=0)
    temp_c: Optional[float]
    rh: Optional[float]
    heat_index_c: Optional[float]
    flame: bool
    door: Literal["LOCKED", "UNLOCKED"]
    status: Literal["SAFE", "WARNING", "DANGER", "FIRE"]
    heat_trend: Literal["RISING", "STABLE", "FALLING"]
    rssi: Optional[int]
    heap_kb: int
    chip_temp_c: Optional[float]
    fw: str = Field(max_length=16)
    quality: Literal["good", "bad"]


class Event(Base):
    type: str = Field(pattern=r"^[A-Z_]{2,32}$")
    detail: str = Field(default="", max_length=256)


# ------------------------------------------------------------------- ingest
def touch_device(t, m, now):
    row = t.rows("SELECT online FROM devices WHERE device_id=%s", (m.device_id,))
    fw = getattr(m, "fw", None)
    if not row:
        t.q("INSERT INTO devices(device_id, room_id, online, last_seen, last_seq, fw) VALUES(%s,%s,%s,%s,%s,%s)",
            (m.device_id, m.room_id, True, now, m.seq, fw))
        came_online = True
    else:
        came_online = not row[0]["online"]
        t.q("UPDATE devices SET room_id=%s, online=%s, last_seen=%s, last_seq=%s, fw=COALESCE(%s, fw) "
            "WHERE device_id=%s", (m.room_id, True, now, m.seq, fw, m.device_id))
    if came_online:
        t.q("INSERT INTO device_state_log(device_id, state, at) VALUES(%s,%s,%s)", (m.device_id, "ONLINE", now))
        log.info("device %s ONLINE", m.device_id)


def ingest(kind, m):
    now = int(time.time())

    def go(t):
        touch_device(t, m, now)
        # The ledger makes (device_id, seq) unique across telemetry AND events.
        cur = t.q("INSERT INTO seq_ledger(device_id, seq, kind, received_at) VALUES(%s,%s,%s,%s) "
                  "ON CONFLICT (device_id, seq) DO NOTHING", (m.device_id, m.seq, kind, now))
        if cur.rowcount == 0:
            t.q("INSERT INTO rejected(device_id, seq, kind, reason, at) VALUES(%s,%s,%s,%s,%s)",
                (m.device_id, m.seq, kind, "replay", now))
            return False
        if kind == "telemetry":
            t.q("INSERT INTO telemetry(device_id, room_id, seq, ts, uptime_s, temp_c, rh, heat_index_c, flame, door,"
                " status, heat_trend, rssi, heap_kb, chip_temp_c, fw, quality, received_at)"
                " VALUES(%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)",
                (m.device_id, m.room_id, m.seq, m.ts, m.uptime_s, m.temp_c, m.rh, m.heat_index_c, m.flame, m.door,
                 m.status, m.heat_trend, m.rssi, m.heap_kb, m.chip_temp_c, m.fw, m.quality, now))
        else:
            t.q("INSERT INTO events(device_id, room_id, seq, ts, type, detail, received_at)"
                " VALUES(%s,%s,%s,%s,%s,%s,%s)", (m.device_id, m.room_id, m.seq, m.ts, m.type, m.detail, now))
        return True

    return db.run(go)


def bearer(request):
    h = request.headers.get("authorization", "")
    return h[7:].strip() if h.lower().startswith("bearer ") else ""


def make_ingest(kind, model):
    async def endpoint(request: Request):
        raw = await request.body()
        if len(raw) > MAX_BODY:
            return JSONResponse({"error": "too large"}, 413)
        try:
            data = json.loads(raw)
        except ValueError:
            return JSONResponse({"error": "invalid json"}, 400)
        if not isinstance(data, dict):
            return JSONResponse({"error": "invalid json"}, 400)

        # Authenticate before anything else: the token must belong to this device_id.
        expected = DEVICE_TOKENS.get(str(data.get("device_id", "")), "")
        token = bearer(request)
        if not expected or not token or not hmac.compare_digest(expected.encode(), token.encode()):
            return JSONResponse({"error": "unauthorized"}, 401)

        try:
            m = model.model_validate(data)
        except ValidationError as e:
            return JSONResponse({"error": "invalid", "detail": json.loads(e.json(include_url=False))}, 422)

        stored = await run_in_threadpool(ingest, kind, m)
        if not stored:
            log.info("replay rejected: %s seq=%s", m.device_id, m.seq)
            return JSONResponse({"error": "replay", "detail": "seq already stored", "seq": m.seq}, 409)
        if kind == "events":
            log.info("event %s %s %s", m.device_id, m.type, m.detail)
        return JSONResponse({"stored": True, "seq": m.seq}, 201)

    return endpoint


# ------------------------------------------------------- offline detection
def mark_offline():
    now = int(time.time())

    def go(t):
        gone = t.rows("UPDATE devices SET online=%s WHERE online=%s AND last_seen < %s RETURNING device_id",
                      (False, True, now - OFFLINE_AFTER_S))
        for g in gone:
            t.q("INSERT INTO device_state_log(device_id, state, at) VALUES(%s,%s,%s)", (g["device_id"], "OFFLINE", now))
            log.info("device %s OFFLINE (silent > %ss)", g["device_id"], OFFLINE_AFTER_S)

    db.run(go)


async def watcher():
    while True:
        try:
            await run_in_threadpool(mark_offline)
        except Exception:
            log.exception("offline watcher error")
        await asyncio.sleep(2)


# ------------------------------------------------- read-only admin endpoints
def admin_only(fn):
    async def wrapper(request: Request):
        token = bearer(request)
        if not ADMIN_TOKEN or not token or not hmac.compare_digest(ADMIN_TOKEN.encode(), token.encode()):
            return JSONResponse({"error": "unauthorized"}, 401)
        return JSONResponse(await run_in_threadpool(fn, request))
    return wrapper


def limit_of(request):
    try:
        return max(1, min(500, int(request.query_params.get("limit", "20"))))
    except ValueError:
        return 20


def table_reader(table):
    def read(request):
        dev = request.query_params.get("device_id")
        where, params = ("WHERE device_id=%s", (dev,)) if dev else ("", ())
        rows = db.run(lambda t: t.rows(f"SELECT * FROM {table} {where} ORDER BY id DESC LIMIT %s",
                                       params + (limit_of(request),)))
        for r in rows:
            if "flame" in r:
                r["flame"] = bool(r["flame"])
        return rows
    return read


def read_devices(request):
    now = int(time.time())
    rows = db.run(lambda t: t.rows("SELECT * FROM devices ORDER BY device_id"))
    for r in rows:
        r["online"] = bool(r["online"])
        r["silent_s"] = now - r["last_seen"]
    return rows


async def healthz(request):
    return JSONResponse({"ok": True, "time": int(time.time())})


async def root(request):
    return JSONResponse({"service": "smartclass", "level": 2})


@contextlib.asynccontextmanager
async def lifespan(app):
    await run_in_threadpool(init_db)
    if not DEVICE_TOKENS:
        log.warning("DEVICE_TOKENS is not set: every device message will be rejected with 401")
    task = asyncio.create_task(watcher())
    yield
    task.cancel()


app = Starlette(
    lifespan=lifespan,
    routes=[
        Route("/", root),
        Route("/healthz", healthz),
        Route("/api/v1/telemetry", make_ingest("telemetry", Telemetry), methods=["POST"]),
        Route("/api/v1/events", make_ingest("events", Event), methods=["POST"]),
        Route("/api/v1/telemetry", admin_only(table_reader("telemetry")), methods=["GET"]),
        Route("/api/v1/events", admin_only(table_reader("events")), methods=["GET"]),
        Route("/api/v1/rejected", admin_only(table_reader("rejected")), methods=["GET"]),
        Route("/api/v1/state-log", admin_only(table_reader("device_state_log")), methods=["GET"]),
        Route("/api/v1/devices", admin_only(read_devices), methods=["GET"]),
    ],
)
