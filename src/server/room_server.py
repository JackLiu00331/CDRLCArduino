"""
CDRLC Study Room Availability Server  v4.1
===========================================
Serves booking availability data for the CDRLC Arduino display board.
Also provides a mobile QR-code booking helper triggered by the Arduino.

Data source: Google Calendar Appointment Scheduling API (no login needed)
Refresh:     Every 5 minutes automatically + on-demand via GET /refresh

Changes in v4.1 (vs v4.0):
  • Fix 2 – Day snapshot: saves each date's last real availability while it is
    still a future date.  When Google Calendar blocks same-day booking and
    returns all rooms as unavailable, /slot falls back to the saved snapshot so
    the display shows meaningful data rather than all-booked.
  • Fix 3 – Weekend dates: Mon–Thu → remaining days of current week;
    Fri–Sun → all of next week (Mon–Fri), so the display is always useful.

HTTP Endpoints – availability (plain text):
  GET /dates               valid viewable dates, comma-separated
  GET /slot?date=YYYYMMDD&time=HHMM   8-char availability string
  GET /hotslots?date=YYYYMMDD         most-booked timeslot(s)
  GET /refresh             trigger an immediate data refresh, returns "OK"
  GET /status              human-readable debug page

HTTP Endpoints – booking helper:
  GET /newbook?date=YYYYMMDD&time=HHMM
                           Arduino calls this to create a session.
                           Returns 6-char plain-text code, e.g. "A1B2C3".

  GET /b/<CODE>            User's phone opens this.  Returns a mobile-friendly
                           HTML page that lists rooms with direct Google Calendar
                           booking links — no proxy, no reCAPTCHA.

Run:  python room_server.py
Set env BOOK_BASE_URL to your public NAS URL, e.g.:
  BOOK_BASE_URL=https://your-nas.ts.net  python room_server.py
"""

from http.server import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime, timedelta, date
from zoneinfo import ZoneInfo
import urllib.request, urllib.parse, json, gzip, threading, time, os, secrets

# ─── Config ───────────────────────────────────────────────────────────────────

CHICAGO_TZ   = ZoneInfo("America/Chicago")
HTTP_PORT    = 8080
REFRESH_SECS = 300

BOOK_BASE_URL = os.environ.get("BOOK_BASE_URL", f"http://localhost:{HTTP_PORT}")

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

# Google Calendar public booking URLs for each room
GCAL_BOOK_URLS = {
    "2432": "https://calendar.google.com/calendar/appointments/schedules/AcZssZ2zV3rqByvVaYY7-waxW0Ua1PFUrJy-geZAjUoquyZkwl-b7k-AVlj-gXtu6hL2fdoNyOW_fogm",
    "2434": "https://calendar.google.com/calendar/appointments/schedules/AcZssZ3qsbG4VZdKujEEomP4ZHcGG9G4DrTlALvL_aiJevQ2RQ1CqCTSox-V5VLmuvL2tx6UihIkAMbK",
    "2436": "https://calendar.google.com/calendar/appointments/schedules/AcZssZ1QyvW6zFu7S-UsVPVzcjZ1BuXTmbqFs1ijAzbAdAUjuyBEK37hNdn60h3GTYWUBGzGHM2tGpq9",
    "2438": "https://calendar.google.com/calendar/appointments/schedules/AcZssZ1HATv8WKSJRkxB09vAt9Kf3--pxITTPS9jiW8sVQ_h5vQvwVSXi2qdcyiUtFhJ7koaQ6YYmn0r",
    "2440": "https://calendar.google.com/calendar/appointments/schedules/AcZssZ1ZA6mIdf3_OSJa22JX9-ziy8iqdcPLFaTjOAKSgvKm3LLnjAMPYiAyOSlVzHHS5jHVlaov9E_l",
    "2426": "https://calendar.google.com/calendar/appointments/schedules/AcZssZ2KJdEXUrjMzwDlPBlhJYrh_kMoGgkZSWfbTMVNhFQJRdlt-cEW5u5v1VpLk_kvXfhkQVIzMa6e",
    "2428": "https://calendar.google.com/calendar/appointments/schedules/AcZssZ0p9czIXJNtpMOf5tXBGmslRJMv64NSx_5D7atsWFCoajdv5GvbMZb_k7alyZ5zxU16KJ1Elsr5",
    "2430": "https://calendar.google.com/calendar/appointments/schedules/AcZssZ0p9czIXJNtpMOf5tXBGmslRJMv64NSx_5D7atsWFCoajdv5GvbMZb_k7alyZ5zxU16KJ1Elsr5",
}

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

# ─── Booking sessions ─────────────────────────────────────────────────────────
booking_sessions: dict[str, dict] = {}
sessions_lock = threading.Lock()
SESSION_TTL = 600   # 10 minutes

# ─── Day snapshot (Fix 2) ─────────────────────────────────────────────────────
# Each time a future date's availability is fetched we save the computed slot
# strings here.  When that date becomes "today", Google Calendar blocks same-day
# booking and returns all rooms as unavailable; we serve the snapshot instead.
#
# Format: day_snapshot[date_str][slot_label] = "10010100"
# Persisted to DAY_SNAPSHOT_FILE so it survives container restarts.
#
# Docker note: to persist across container recreations, mount a host directory
# at /data and set DAY_SNAPSHOT_FILE = "/data/day_snapshot.json".
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
        print(f"  [snapshot] No existing snapshot – starting fresh")


def save_day_snapshot():
    """Persist snapshot; also prune entries older than 7 days."""
    cutoff = (datetime.now(CHICAGO_TZ).date() - timedelta(days=7)).strftime("%Y%m%d")
    stale  = [k for k in day_snapshot if k < cutoff]
    for k in stale:
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

      Mon–Thu  →  today through Friday of this week (2–5 dates).
                  Today is included even though Google blocks same-day booking;
                  /slot falls back to the day snapshot saved the evening before
                  (Fix 2), so the display still shows meaningful data.

      Friday   →  today (served from Thursday's snapshot via Fix 2)
                  + next Monday through Thursday (4 days).
                  Total: 5 dates.  Dropping next Friday keeps the list at 5
                  so the Arduino's dateList[5] array is never overflowed.

      Sat–Sun  →  all of next week, Monday through Friday (5 dates).
                  No snapshot needed — these are all future dates that the
                  server fetches fresh from Google Calendar.
    """
    today = datetime.now(CHICAGO_TZ).date()
    dow   = today.weekday()          # 0=Mon … 4=Fri, 5=Sat, 6=Sun

    if dow <= 3:                      # Mon–Thu: rest of this week
        monday = today - timedelta(days=dow)
        return [
            (monday + timedelta(days=i)).strftime("%Y%m%d")
            for i in range(5)
            if monday + timedelta(days=i) >= today
        ]
    elif dow == 4:                    # Friday: today (snapshot) + next Mon–Thu
        next_monday = today + timedelta(days=3)
        return (
            [today.strftime("%Y%m%d")] +
            [(next_monday + timedelta(days=i)).strftime("%Y%m%d") for i in range(4)]
        )
    else:                             # Sat–Sun: all of next week
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
              "  ".join(f"{SLOT_LABELS[i]}→{free_counts[i]}free"
                        for i in range(len(STANDARD_SLOTS))))

        # ── Fix 2: build day snapshot for future dates ─────────────────────
        # We do NOT save today's data (Google returns all-blocked for same-day).
        # We DO save every future date so tomorrow's display has real data.
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


# ─── Session helpers ──────────────────────────────────────────────────────────

def make_code() -> str:
    return secrets.token_hex(3).upper()   # e.g. "A1B2C3"


def cleanup_sessions():
    cutoff = time.time() - SESSION_TTL
    with sessions_lock:
        for k in [k for k, v in booking_sessions.items() if v["created"] < cutoff]:
            del booking_sessions[k]


# ─── Query helpers ────────────────────────────────────────────────────────────

def get_slot_string(date_str: str, time_str: str) -> str:
    """
    Return 8-char availability string for one date+slot combination.
    '0' = free, '1' = booked.

    Fix 2: if the requested date is today, Google blocks same-day booking and
    returns all rooms as unavailable.  We serve the day snapshot saved when
    that date was still a future date, so the display shows real room state.
    """
    today_str = datetime.now(CHICAGO_TZ).date().strftime("%Y%m%d")

    if date_str == today_str and date_str in day_snapshot:
        result = day_snapshot[date_str].get(time_str, "11111111")
        return result

    # Normal live cache lookup
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
    day_data     = week_cache[date_str]
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
    hot = [label for count, label in booked_counts if count == max_booked]
    return ",".join(hot)


# ─── Booking room-list page HTML ──────────────────────────────────────────────

def booking_page_html(session: dict) -> str:
    date_str = session["date"]
    time_str = session["time"]

    d_obj    = datetime.strptime(date_str, "%Y%m%d")
    day_disp = d_obj.strftime("%a, %b %d")
    h_start  = int(time_str[:2])
    h_end    = h_start + 1
    t_disp   = f"{h_start:02d}:{time_str[2:]} – {h_end:02d}:{time_str[2:]}"

    slot_str  = get_slot_string(date_str, time_str)

    rows_html = ""
    for i, room in enumerate(ROOM_ORDER):
        taken = (slot_str[i] == "1") if i < len(slot_str) else True
        gcal  = GCAL_BOOK_URLS.get(room, "")
        if taken:
            rows_html += f"""
      <div class="room-row taken">
        <span class="room-num">Room {room}</span>
        <span class="status-tag taken-tag">Taken</span>
      </div>"""
        else:
            rows_html += f"""
      <a class="room-row free" href="{gcal}" target="_blank">
        <span class="room-num">Room {room}</span>
        <span class="status-tag free-tag">Book →</span>
      </a>"""

    free_count = slot_str.count("0") if "?" not in slot_str else "?"

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Book a Study Room</title>
  <style>
    * {{ box-sizing: border-box; margin: 0; padding: 0; }}
    body {{
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background: #f0f4f8;
      min-height: 100vh;
      padding: 1.25rem;
    }}
    .header {{
      background: #fff;
      border-radius: 16px;
      padding: 1.25rem 1.5rem;
      margin-bottom: 1rem;
      box-shadow: 0 2px 12px rgba(0,0,0,.08);
    }}
    .header h1 {{ font-size: 1rem; color: #888; font-weight: 500; margin-bottom: .3rem; }}
    .slot-label {{ font-size: 1.4rem; font-weight: 700; color: #1a1a2e; }}
    .free-count {{ margin-top: .4rem; font-size: .9rem; color: #27ae60; font-weight: 600; }}
    .list {{ display: flex; flex-direction: column; gap: .6rem; }}
    .room-row {{
      display: flex;
      align-items: center;
      justify-content: space-between;
      background: #fff;
      border-radius: 12px;
      padding: 1rem 1.25rem;
      box-shadow: 0 2px 8px rgba(0,0,0,.06);
      text-decoration: none;
      color: inherit;
    }}
    .room-row.free  {{ border-left: 4px solid #27ae60; cursor: pointer; }}
    .room-row.free:active {{ background: #f0fff4; }}
    .room-row.taken {{ border-left: 4px solid #e0e0e0; opacity: .55; }}
    .room-num {{ font-size: 1.1rem; font-weight: 600; color: #1a1a2e; }}
    .status-tag {{ font-size: .85rem; font-weight: 600; padding: .25rem .75rem; border-radius: 999px; }}
    .free-tag  {{ background: #e8f5e9; color: #27ae60; }}
    .taken-tag {{ background: #f5f5f5; color: #aaa; }}
    .footer {{ margin-top: 1.25rem; text-align: center; font-size: .75rem; color: #bbb; }}
  </style>
</head>
<body>
  <div class="header">
    <h1>CDRLC Study Rooms</h1>
    <div class="slot-label">{day_disp} · {t_disp}</div>
    <div class="free-count">{free_count} room{'s' if free_count != 1 else ''} available</div>
  </div>
  <div class="list">{rows_html}
  </div>
  <p class="footer">Tap a green room to open Google Calendar and complete your booking.</p>
</body>
</html>"""


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

    def reply_html(self, html: str, code: int = 200):
        self.reply(html, code, "text/html; charset=utf-8")

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
            if len(d) == 8 and len(t) == 4:
                self.reply(get_slot_string(d, t))
            else:
                self.reply("ERR: need ?date=YYYYMMDD&time=HHMM", 400)

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
                "CDRLC Room Availability Server v4.1\n",
                "=" * 42 + "\n",
                f"Server time  : {now.strftime('%Y-%m-%d %H:%M:%S %Z')}\n",
                f"Last refresh : {ts}\n",
                f"Cached dates : {', '.join(week_cache.keys()) or 'none'}\n",
                f"Snapshot dates: {', '.join(day_snapshot.keys()) or 'none'}\n\n",
            ]
            for date_str in valid_dates():
                d_obj = datetime.strptime(date_str, "%Y%m%d")
                today_str = now.date().strftime("%Y%m%d")
                src = "(snapshot)" if date_str == today_str and date_str in day_snapshot else ""
                lines.append(f"  {d_obj.strftime('%a %m/%d')}  ({date_str}) {src}\n")
                for (h, m), label in zip(STANDARD_SLOTS, SLOT_LABELS):
                    row  = get_slot_string(date_str, label)
                    free = row.count("0")
                    rooms_free = [ROOM_ORDER[i] for i in range(8) if row[i] == "0"]
                    lines.append(
                        f"    {h:02d}:{m:02d}  [{row}]  "
                        f"{free} free: {', '.join(rooms_free) if rooms_free else 'none'}\n"
                    )
                if date_str in week_cache:
                    hot = get_hot_slots(date_str)
                    lines.append(f"    Hot slots: {hot}\n")
                lines.append("\n")
            self.reply("".join(lines))

        # ── /newbook?date=YYYYMMDD&time=HHMM  (Arduino) ────────────────────
        elif path == "/newbook":
            d = params.get("date", "")
            t = params.get("time", "")
            if len(d) != 8 or len(t) != 4:
                self.reply("ERR: need ?date=YYYYMMDD&time=HHMM", 400)
                return
            cleanup_sessions()
            code = make_code()
            with sessions_lock:
                booking_sessions[code] = {
                    "date":    d,
                    "time":    t,
                    "created": time.time(),
                }
            print(f"[newbook] {code}  {d} {t}")
            self.reply(code)

        # ── /b/<CODE>  (user's phone) ────────────────────────────────────────
        elif path.startswith("/b/"):
            code = path[3:].strip("/").upper()
            with sessions_lock:
                session = booking_sessions.get(code)
            if not session:
                self.reply_html(
                    "<html><body style='font-family:sans-serif;padding:2rem'>"
                    "<h2>⏱ Link expired</h2>"
                    "<p>This booking link has expired (10 min limit).<br>"
                    "Please press the button again on the Arduino display.</p>"
                    "</body></html>", 404)
                return
            self.reply_html(booking_page_html(session))

        else:
            self.reply("Not found", 404)

    def log_message(self, fmt, *args):
        if self.path not in ("/slot", "/dates"):
            super().log_message(fmt, *args)


# ─── Main ─────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    print("=" * 50)
    print("  CDRLC Room Availability Server  v4.1")
    print("=" * 50)
    print()
    print(f"  Book base URL : {BOOK_BASE_URL}")
    print()

    load_day_snapshot()   # restore snapshot from previous run before first refresh
    refresh_all()

    threading.Thread(target=refresh_loop, daemon=True).start()

    print(f"\nListening on 0.0.0.0:{HTTP_PORT}")
    print(f"  GET /dates                         valid dates (week-aware)")
    print(f"  GET /slot?date=YYYYMMDD&time=HHMM  8-char availability")
    print(f"  GET /hotslots?date=YYYYMMDD         most booked timeslot(s)")
    print(f"  GET /refresh                        force re-fetch")
    print(f"  GET /status                         full debug view")
    print(f"  GET /newbook?date=YYYYMMDD&time=HHMM  Arduino: create booking session")
    print(f"  GET /b/<CODE>                       Phone: show room list\n")

    server = HTTPServer(("0.0.0.0", HTTP_PORT), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
