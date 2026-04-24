"""
Step 1: Resolve each room's short booking link to get its Service ID.
The Service ID is the last path segment of the full appointment URL.

Run: python get_service_ids.py
It will print a ready-to-paste SERVICE_IDS dict for room_server.py.
"""

import urllib.request
import urllib.error
import re

# Short links from the CDRLC booking page
SHORT_LINKS = {
    "2432": "https://calendar.google.com/calendar/u/0/appointments/schedules/AcZssZ2zV3rqByvVaYY7-waxW0Ua1PFUrJy-geZAjUoquyZkwl-b7k-AVlj-gXtu6hL2fdoNyOW_fogm",
    "2434": "https://calendar.app.google/HBRMVA9qjTHhmVDi9",
    "2436": "https://calendar.app.google/CDKX6Gtqf5ctTbdL8",
    "2438": "https://calendar.app.google/V24dAX3fMYiZHJb4A",
    "2440": "https://calendar.app.google/wxmijyH1M9E6qe9j7",
    "2426": "https://calendar.app.google/CcyzhE51AuA2J1raA",
    "2428": "https://calendar.app.google/SNGcLDhsiFWxXfrX6",
    "2430": "https://calendar.app.google/SNGcLDhsiFWxXfrX6",
}

def resolve_service_id(room, url):
    """Follow redirects and extract the service ID from the final URL."""
    # If it's already a long appointments URL, just extract the ID
    m = re.search(r'/appointments/schedules/([A-Za-z0-9_\-]+)', url)
    if m:
        return m.group(1)

    # Follow the short link redirect
    try:
        req = urllib.request.Request(
            url,
            headers={"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/147.0.0.0"},
            method="GET"
        )
        # Don't follow redirects automatically — we want the Location header
        opener = urllib.request.build_opener(urllib.request.HTTPRedirectHandler())
        # Actually follow redirects to get final URL
        resp = opener.open(req, timeout=10)
        final_url = resp.geturl()
        m = re.search(r'/appointments/schedules/([A-Za-z0-9_\-]+)', final_url)
        if m:
            return m.group(1)
        # Sometimes the ID is in the page HTML
        html = resp.read().decode("utf-8", errors="ignore")
        m = re.search(r'/appointments/schedules/([A-Za-z0-9_\-]{20,})', html)
        if m:
            return m.group(1)
        print(f"  [!] Could not find service ID in final URL: {final_url}")
        return None
    except Exception as e:
        print(f"  [!] Error resolving {url}: {e}")
        return None


print("Resolving service IDs for all rooms...\n")
service_ids = {}

for room, url in SHORT_LINKS.items():
    sid = resolve_service_id(room, url)
    service_ids[room] = sid
    status = sid[:40] + "..." if sid else "FAILED"
    print(f"  Room {room}: {status}")

print()
print("=" * 60)
print("Paste this into room_server.py as SERVICE_IDS:")
print("=" * 60)
print("SERVICE_IDS = {")
for room, sid in service_ids.items():
    if sid:
        print(f'    "{room}": "{sid}",')
    else:
        print(f'    "{room}": None,  # TODO: manually find this ID')
print("}")
