"""
prefetch.py  ─  Run this BEFORE MIDNIGHT the night before your demo.
================================================================
Queries ListAvailableSlots for TOMORROW and saves the result to
today_cache.json.  On demo day, room_server.py reads this file
to show correct room availability without needing live API access.

Run once the evening before:
    python prefetch.py

Example:  demo is April 27  →  run this on April 26 at ~10 PM.
"""

import urllib.request
import urllib.parse
import json
import gzip
from datetime import datetime, timedelta, timezone, date
from zoneinfo import ZoneInfo

# ─── Shared config (same as room_server.py) ───────────────────────────────────
API_KEY  = "AIzaSyA7GKm43l8WNxlLTjsldq9z9n80CL6KW4U"
BASE_URL = ("https://calendar-pa.clients6.google.com"
            "/$rpc/google.internal.calendar.v1"
            ".AppointmentBookingService/ListAvailableSlots")
CHICAGO_TZ = ZoneInfo("America/Chicago")

SLOT_START_HOUR, SLOT_START_MINUTE = 8, 30
SLOTS_PER_DAY = 8

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
ROOM_ORDER = ["2432", "2434", "2436", "2438", "2440", "2426", "2428", "2430"]


def get_standard_slot_timestamps(target_date):
    """All 8 standard slot start-times (UTC unix ts) for a given Chicago date."""
    slots = []
    for i in range(SLOTS_PER_DAY):
        local = datetime(target_date.year, target_date.month, target_date.day,
                         SLOT_START_HOUR, SLOT_START_MINUTE,
                         tzinfo=CHICAGO_TZ) + timedelta(hours=i)
        slots.append(int(local.timestamp()))
    return slots


def fetch_available_timestamps(service_id, start_ts, end_ts):
    http_headers = (
        f"X-Goog-Api-Key:{API_KEY}\r\n"
        "Content-Type:application/json+protobuf\r\n"
        "X-User-Agent:grpc-web-javascript/0.1\r\n"
    )
    params = urllib.parse.urlencode({"$httpHeaders": http_headers})
    url    = f"{BASE_URL}?{params}"
    body   = json.dumps([None, None, service_id, None,
                         [[start_ts], [end_ts]]]).encode("utf-8")
    req = urllib.request.Request(
        url, data=body,
        headers={
            "Content-Type":    "application/x-www-form-urlencoded;charset=UTF-8",
            "Origin":          "https://calendar.google.com",
            "Referer":         "https://calendar.google.com/",
            "User-Agent":      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/147.0.0.0",
            "Accept-Encoding": "gzip, deflate",
        },
        method="POST"
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            raw = resp.read()
            if resp.headers.get("Content-Encoding") == "gzip":
                raw = gzip.decompress(raw)
        data = json.loads(raw.decode("utf-8"))
        available = set()
        # data may be [] (all slots booked) or [[...]] (some available)
        if data and data[0]:
            for slot_group in data[0]:
                for slot in slot_group:
                    try:
                        available.add(int(slot[0][0]))
                    except (IndexError, TypeError, ValueError):
                        pass
        return available   # empty set = all slots taken (still valid, not an error)
    except Exception as e:
        print(f"      API error: {e}")
        return None        # None = real network/parse error


def main():
    now_chicago   = datetime.now(CHICAGO_TZ)
    target_date   = (now_chicago + timedelta(days=1)).date()   # TOMORROW
    window_start  = int(datetime(target_date.year, target_date.month,
                                 target_date.day, tzinfo=CHICAGO_TZ).timestamp())
    window_end    = window_start + 86400

    print("=" * 52)
    print("  CDRLC Pre-fetch  ─  run the night before demo")
    print("=" * 52)
    print(f"  Current time : {now_chicago.strftime('%Y-%m-%d %H:%M %Z')}")
    print(f"  Fetching for : {target_date}  (tomorrow / demo day)")
    print()

    all_slots = get_standard_slot_timestamps(target_date)
    cache = {}       # room_id -> list of booked slot timestamps

    for room in ROOM_ORDER:
        sid = SERVICE_IDS.get(room)
        print(f"  Room {room} ...", end=" ", flush=True)
        available = fetch_available_timestamps(sid, window_start, window_end)
        if available is None:
            print("ERROR (will default to free)")
            cache[room] = []
            continue
        booked = [ts for ts in all_slots if ts not in available]
        cache[room] = booked
        slot_times = []
        for ts in booked:
            t = datetime.fromtimestamp(ts, tz=CHICAGO_TZ)
            slot_times.append(t.strftime("%I:%M %p"))
        print(f"{len(booked)} booked  {slot_times if slot_times else '(all free)'}")

    cache["_date"]     = str(target_date)
    cache["_fetched"]  = now_chicago.isoformat()
    cache["_all_slots"] = {str(target_date): all_slots}

    with open("F:\\Arduino\\Project\\today_cache.json", "w") as f:
        json.dump(cache, f, indent=2)

    print()
    print("Saved to today_cache.json")
    print("Run room_server.py tomorrow to serve this data to Arduino.")
    print()
    print("Preview for tomorrow:")
    print(f"  {'Room':<8}  {'Booked slots'}")
    print(f"  {'-'*7}  {'-'*35}")
    for room in ROOM_ORDER:
        booked_ts = cache.get(room, [])
        if booked_ts:
            times = ", ".join(
                datetime.fromtimestamp(ts, tz=CHICAGO_TZ).strftime("%I:%M%p")
                for ts in sorted(booked_ts)
            )
        else:
            times = "(none booked)"
        print(f"  {room:<8}  {times}")


if __name__ == "__main__":
    main()
