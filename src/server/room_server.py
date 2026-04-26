from __future__ import annotations  # allow X | Y union syntax on Python < 3.10

# CDRLC Study Room Availability Server  v4.2
#
# Serves room availability data for the CDRLC Arduino display board.
# Arduino generates booking QR codes locally using hard-coded Google Calendar
# URLs — no server-side booking session management needed.
#
# Data source: Google Calendar Appointment Scheduling API (no login needed)
# Refresh:     Every 5 minutes automatically + on-demand via GET /refresh
#
# HTTP Endpoints:
#   GET /dates                          valid viewable dates, comma-separated
#   GET /slot?date=YYYYMMDD&time=HHMM   8-char availability string
#   GET /allslots?date=YYYYMMDD         all 8 slots comma-separated (bulk prefetch)
#   GET /hotslots?date=YYYYMMDD         most-booked timeslot label(s)
#   GET /refresh                        trigger immediate data refresh, returns "OK"
#   GET /status                         human-readable debug page
#
# Run:  python room_server.py

from http.server import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime, timedelta
from zoneinfo import ZoneInfo
import urllib.request, urllib.parse, json, gzip, threading, time, os

# ─── Config ───────────────────────────────────────────────────────────────────

CHICAGO_TZ   = ZoneInfo("America/Chicago")
HTTP_PORT    = 8080
REFRESH_SECS = 300

STANDARD_SLOTS = [
    (9,30),(10,30),(11,30),(12,30),(13,30),(14,30),(15,30),(16,30)
]
SLOT_LABELS = ["0930","1030","1130","1230","1330","1430","1530","1630"]

SLOT_TS_OFFSET = -3600

ROOM_ORDER = ["2432","2434","2436","2438","2440","2426","2428","2430"]

API_KEY  = "AIzaSyA7GKm43l8WNxlLTjsldq9z9n80CL6KW4U"
BASE_URL = ("https://calendar-pa.clients6.google.com"
            "/$rpc/google.internal.calendar.v1"
            ".AppointmentBookingService/ListAvailableSlots")

# Google Calendar appointment scheduling service IDs (used for API calls)
SERVICE_IDS = {
    "2432": "AcZssZ2zV3rqByvVaYY7-waxW0Ua1PFUrJy-geZAjUoquyZkwl-b7k-AVlj-gXtu6hL2fdoNyOW_fogm",
    "2434": "AcZssZ3qsbG4VZdKujEEomP4ZHcGG9G4DrTlALvL_aiJevQ2RQ1CqCTSox-V5VLmuvL2tx6UihIkAMbK",
    "2436": "AcZssZ1QyvW6zFu7S-UsVPVzcjZ1BuXTmbqFs1ijAzbAdAUjuyBEK37hNdn60h3GTYWUBGzGHM2tGpq9",
    "2438": "AcZssZ1HATv8WKSJRkxB09vAt9Kf3--pxITTPS9jiW8sVQ_h5vQvwVSXi2qdcyiUtFhJ7koaQ6YYmn0r",
    "2440": "AcZssZ1ZA6mIdf3_OSJa22JX9-ziy8iqdcPLFaTjOAKSgvKm3LLnjAMPYiAyOSlVzHHS5jHVlaov9E_l",
    "2426": "AcZssZ2KJdEXUrjMzwDlPBlhJYrh_kMoGgkZSWfbTMVNhFQJRdlt-cEW5u5v1VpLk_kvXfhkQVIzMa6e",
    "2428": "AcZssZ0p9czIXJNtpMOf5tXBGmslRJMv64NSx_5D7atsWFCoajdv5GvbMZb_k7alyZ5zxU16KJ1Elsr5",
    "2430": "AcZssZ0p9czIXJNtpMOf5tXBGmslRJMv64NSx_5D7atsWFCoajdv5GvbMZb_k7alyZ5zxU16KJ1Elsr5",
}

# ─── Global cache ─────────────────────────────────────────────────────────────
week_cache: dict[str, dict[str, set]] = {}
last_refresh: datetime | None = None
refresh_lock = threading.Lock()

# ─── Day snapshot ─────────────────────────────────────────────────────────────
# Saves each future date's availability so that when the date becomes "today"
# (and Google blocks same-day booking), /slot can still return real data.
# Format: day_snapshot[date_str][slot_label] = "10010100"
# Persisted to DAY_SNAPSHOT_FILE so it survives container restarts.
DAY_SNAPSHOT_FILE = "day_snapshot.json"
day_snapshot: dict[str, dict[str, str]] = {}


def load_day_snapshot():
    global day_snapshot
    try:
        with open(DAY_SNAPSHOT_FILE) as f:
            day_snapshot = json.load(f)
        print(f"  [snapshot] Loaded {len(day_snapshot)} date(s) from {DAY_SNAPSHOT_FILE}")
    except (FileNotFoundError, json.JSONDecodeError):
        day_snapshot = {}
        print(f"  [snapshot] No existing snapshot — starting fresh")


def save_day_snapshot():
    cutoff = (datetime.now(CHICAGO_TZ).date() - timedelta(days=7)).strftime("%Y%m%d")
    for k in [k for k in day_snapshot if k < cutoff]:
        del day_snapshot[k]
    try:
        with open(DAY_SNAPSHOT_FILE, "w") as f:
            json.dump(day_snapshot, f)
    except Exception as e:
        print(f"  [snapshot] Save failed: {e}")


# ─── Date helpers ─────────────────────────────────────────────────────────────

def valid_dates() -> list[str]:
    """
    Date list rules:

      Mon-Thu  ->  today through Friday of this week (2-5 dates).
                   Today is included; /slot serves the day snapshot for today.

      Friday   ->  today (snapshot) + next Monday through Thursday (4 days).
                   Total: 5 dates. Next Friday is dropped to stay within the
                   Arduino's dateList[5] array limit.

      Sat-Sun  ->  all of next week, Monday through Friday (5 dates).
    """
    today = datetime.now(CHICAGO_TZ).date()
    dow   = today.weekday()          # 0=Mon ... 4=Fri, 5=Sat, 6=Sun

    if dow <= 3:                      # Mon-Thu: rest of this week
        monday = today - timedelta(days=dow)
        return [
            (monday + timedelta(days=i)).strftime("%Y%m%d")
            for i in range(5)
            if monday + timedelta(days=i) >= today
        ]
    elif dow == 4:                    # Friday: today (snapshot) + next Mon-Thu
        next_monday = today + timedelta(days=3)
        return (
            [today.strftime("%Y%m%d")] +
            [(next_monday + timedelta(days=i)).strftime("%Y%m%d") for i in range(4)]
        )
    else:                             # Sat-Sun: all of next week
        days_ahead  = (7 - dow) % 7 or 7
        next_monday = today + timedelta(days=days_ahead)
        return [(next_monday + timedelta(days=i)).strftime("%Y%m%d") for i in range(5)]


def slot_utc_ts(date_str: str, hour: int, minute: int) -> int:
    d = datetime.strptime(date_str, "%Y%m%d")
    local_dt = datetime(d.year, d.month, d.day, hour, minute,
                        tzinfo=CHICAGO_TZ)
    return int(local_dt.timestamp()) + SLOT_TS_OFFSET


# ─── API call ─────────────────────────────────────────────────────────────────

def fetch_available_ts(service_id: str, window_start: int, window_end: int) -> set | None:
    http_headers = (
        f"X-Goog-Api-Key:{API_KEY}\r\n"
        "Content-Type:application/json+protobuf\r\n"
        "X-User-Agent:grpc-web-javascript/0.1\r\n"
    )
    params = urllib.parse.urlencode({"$httpHeaders": http_headers})
    url    = f"{BASE_URL}?{params}"
    body   = json.dumps([None, None, service_id, None,
                         [[window_start],[window_end]]]).encode()
    req = urllib.request.Request(url, data=body, method="POST",
        headers={
            "Content-Type":    "application/x-www-form-urlencoded;charset=UTF-8",
            "Origin":          "https://calendar.google.com",
            "Referer":         "https://calendar.google.com/",
            "User-Agent":      "Mozilla/5.0 Chrome/147.0.0.0",
            "Accept-Encoding": "gzip, deflate",
        })
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            raw = resp.read()
            if resp.headers.get("Content-Encoding") == "gzip":
                raw = gzip.decompress(raw)
        data = json.loads(raw.decode())
        available = set()
        if data and data[0]:
            for slot_group in data[0]:
                for sg in slot_group:
                    try:
                        available.add(int(sg[0][0]))
                    except (IndexError, TypeError, ValueError):
                        pass
        return available
    except Exception as e:
        print(f"    [API] {service_id[:12]}... error: {e}")
        return None


# ─── Refresh logic ────────────────────────────────────────────────────────────

def refresh_all():
    global week_cache, last_refresh
    today_str   = datetime.now(CHICAGO_TZ).date().strftime("%Y%m%d")
    dates       = valid_dates()
    now_chicago = datetime.now(CHICAGO_TZ)

    print(f"\n[{now_chicago.strftime('%H:%M:%S')}] Refreshing "
          f"{len(dates)} dates x {len(ROOM_ORDER)} rooms ...")

    new_cache: dict[str, dict[str, set]] = {}

    for date_str in dates:
        new_cache[date_str] = {}
        d = datetime.strptime(date_str, "%Y%m%d").date()
        day_start = int(datetime(d.year, d.month, d.day, 0, 0,
                                 tzinfo=CHICAGO_TZ).timestamp())
        day_end = day_start + 86400

        for room in ROOM_ORDER:
            sid       = SERVICE_IDS.get(room)
            available = fetch_available_ts(sid, day_start, day_end)
            new_cache[date_str][room] = available if available is not None else set()

        free_counts = []
        for h, m in STANDARD_SLOTS:
            ts   = slot_utc_ts(date_str, h, m)
            free = sum(1 for r in ROOM_ORDER
                       if ts in new_cache[date_str].get(r, set()))
            free_counts.append(free)

        d_obj = datetime.strptime(date_str, "%Y%m%d")
        print(f"  {d_obj.strftime('%a %m/%d')}:  " +
              "  ".join(f"{SLOT_LABELS[i]}->{free_counts[i]}free"
                        for i in range(len(STANDARD_SLOTS))))

        # Save snapshot for future dates (not today — Google returns all-blocked)
        if date_str != today_str:
            snap = {}
            for slot_label, (h, m) in zip(SLOT_LABELS, STANDARD_SLOTS):
                ts     = slot_utc_ts(date_str, h, m)
                result = "".join(
                    "0" if ts in new_cache[date_str].get(r, set()) else "1"
                    for r in ROOM_ORDER
                )
                snap[slot_label] = result
            day_snapshot[date_str] = snap
            print(f"    [snapshot] Saved {date_str}")

    save_day_snapshot()

    with refresh_lock:
        week_cache   = new_cache
        last_refresh = now_chicago
    print(f"  Done. Cache covers: {', '.join(dates)}")


def refresh_loop():
    while True:
        time.sleep(REFRESH_SECS)
        refresh_all()


# ─── Query helpers ────────────────────────────────────────────────────────────

def get_slot_string(date_str: str, time_str: str) -> str:
    # If today's date is requested, Google blocks same-day booking and returns
    # all rooms unavailable.  Serve the snapshot saved while it was a future date.
    today_str = datetime.now(CHICAGO_TZ).date().strftime("%Y%m%d")
    if date_str == today_str and date_str in day_snapshot:
        return day_snapshot[date_str].get(time_str, "11111111")

    if not week_cache:
        return "????????"
    hour     = int(time_str[:2])
    minute   = int(time_str[2:])
    ts       = slot_utc_ts(date_str, hour, minute)
    day_data = week_cache.get(date_str, {})
    return "".join(
        "0" if ts in day_data.get(room, set()) else "1"
        for room in ROOM_ORDER
    )


def get_hot_slots(date_str: str) -> str:
    if not week_cache or date_str not in week_cache:
        return "none"
    day_data      = week_cache[date_str]
    booked_counts = []
    for (h, m), label in zip(STANDARD_SLOTS, SLOT_LABELS):
        ts     = slot_utc_ts(date_str, h, m)
        booked = sum(1 for room in ROOM_ORDER
                     if ts not in day_data.get(room, set()))
        booked_counts.append((booked, label))
    if not booked_counts:
        return "none"
    max_booked = max(c for c, _ in booked_counts)
    if max_booked == 0:
        return "none"
    return ",".join(label for count, label in booked_counts if count == max_booked)


# ─── HTTP handler ─────────────────────────────────────────────────────────────

class Handler(BaseHTTPRequestHandler):

    def reply(self, body: str, code: int = 200,
              content_type: str = "text/plain; charset=utf-8"):
        b = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        params = dict(urllib.parse.parse_qsl(parsed.query))
        path   = parsed.path

        # ── /dates ──────────────────────────────────────────────────────────
        if path == "/dates":
            self.reply(",".join(valid_dates()))

        # ── /slot?date=YYYYMMDD&time=HHMM ───────────────────────────────────
        elif path == "/slot":
            d = params.get("date", "")
            t = params.get("time", "")
            if len(d) == 8 and d.isdigit() and len(t) == 4 and t.isdigit():
                self.reply(get_slot_string(d, t))
            else:
                self.reply("ERR: need ?date=YYYYMMDD&time=HHMM", 400)

        # ── /allslots?date=YYYYMMDD  (Arduino bulk prefetch) ────────────────
        elif path == "/allslots":
            d = params.get("date", "")
            if len(d) != 8 or not d.isdigit():
                self.reply("ERR: need ?date=YYYYMMDD", 400)
                return
            # All 8 slots comma-separated: "10010100,11111111,..."  (~71 chars)
            # Arduino fetches this once per date to fill its entire cache.
            self.reply(",".join(get_slot_string(d, label) for label in SLOT_LABELS))

        # ── /hotslots?date=YYYYMMDD ─────────────────────────────────────────
        elif path == "/hotslots":
            d = params.get("date", "")
            if len(d) == 8:
                self.reply(get_hot_slots(d))
            else:
                self.reply("ERR: need ?date=YYYYMMDD", 400)

        # ── /refresh ────────────────────────────────────────────────────────
        elif path == "/refresh":
            threading.Thread(target=refresh_all, daemon=True).start()
            self.reply("OK")

        # ── /status ─────────────────────────────────────────────────────────
        elif path == "/status":
            now = datetime.now(CHICAGO_TZ)
            ts  = last_refresh.strftime("%Y-%m-%d %H:%M:%S %Z") if last_refresh else "never"
            lines = [
                "CDRLC Room Availability Server v4.2\n",
                "=" * 42 + "\n",
                f"Server time  : {now.strftime('%Y-%m-%d %H:%M:%S %Z')}\n",
                f"Last refresh : {ts}\n",
                f"Cached dates : {', '.join(week_cache.keys()) or 'none'}\n",
                f"Snapshot dates: {', '.join(day_snapshot.keys()) or 'none'}\n\n",
            ]
            today_str = now.date().strftime("%Y%m%d")
            for date_str in valid_dates():
                d_obj = datetime.strptime(date_str, "%Y%m%d")
                src   = " (snapshot)" if date_str == today_str and date_str in day_snapshot else ""
                lines.append(f"  {d_obj.strftime('%a %m/%d')}  ({date_str}){src}\n")
                for (h, m), label in zip(STANDARD_SLOTS, SLOT_LABELS):
                    row        = get_slot_string(date_str, label)
                    free       = row.count("0")
                    rooms_free = [ROOM_ORDER[i] for i in range(8) if row[i] == "0"]
                    lines.append(
                        f"    {h:02d}:{m:02d}  [{row}]  "
                        f"{free} free: {', '.join(rooms_free) if rooms_free else 'none'}\n"
                    )
                if date_str in week_cache:
                    lines.append(f"    Hot slots: {get_hot_slots(date_str)}\n")
                lines.append("\n")
            self.reply("".join(lines))

        else:
            self.reply("Not found", 404)

    def log_message(self, fmt, *args):
        # Suppress high-frequency Arduino poll endpoints to keep logs readable
        if self.path.startswith(("/slot", "/dates", "/allslots")):
            return
        super().log_message(fmt, *args)


# ─── Main ─────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    print("=" * 50)
    print("  CDRLC Room Availability Server  v4.2")
    print("=" * 50)

    load_day_snapshot()
    refresh_all()

    threading.Thread(target=refresh_loop, daemon=True).start()

    print(f"\nListening on 0.0.0.0:{HTTP_PORT}")
    print(f"  GET /dates                         valid dates (week-aware)")
    print(f"  GET /slot?date=YYYYMMDD&time=HHMM  8-char availability string")
    print(f"  GET /allslots?date=YYYYMMDD        all 8 slots (Arduino bulk prefetch)")
    print(f"  GET /hotslots?date=YYYYMMDD        most-booked timeslot(s)")
    print(f"  GET /refresh                       force re-fetch")
    print(f"  GET /status                        full debug view\n")

    server = HTTPServer(("0.0.0.0", HTTP_PORT), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
