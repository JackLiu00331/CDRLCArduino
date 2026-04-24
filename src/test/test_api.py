"""
Quick test: can we call ListAvailableSlots with only the API key?
(No Google session / SAPISIDHASH needed?)

Run: python test_api.py
"""

import urllib.request
import urllib.parse
import json
from datetime import datetime, timezone, timedelta

# Public API key extracted from the booking page (embedded in the JS)
API_KEY = "AIzaSyA7GKm43l8WNxlLTjsldq9z9n80CL6KW4U"

# The $httpHeaders param — try with API key only, no Authorization
HTTP_HEADERS_ANON = "\r\n".join([
    f"X-Goog-Api-Key:{API_KEY}",
    "Content-Type:application/json+protobuf",
    "X-User-Agent:grpc-web-javascript/0.1",
    "",  # trailing CRLF
])

# URL for ListAvailableSlots
BASE_URL = "https://calendar-pa.clients6.google.com/$rpc/google.internal.calendar.v1.AppointmentBookingService/ListAvailableSlots"

# ──────────────────────────────────────────────────────────────────────────────
# NOTE: We need the POST body (booking page service IDs).
# This script will be updated once you share the Payload tab contents.
# For now we test if the endpoint accepts requests without Authorization.
# ──────────────────────────────────────────────────────────────────────────────

# Time window: today → +7 days (Unix seconds)
now_ts = int(datetime.now(timezone.utc).timestamp())
week_ts = now_ts + 7 * 86400

# Placeholder POST body — will be replaced with real proto once Payload is known
# Format is JSON+protobuf, looks like: [[service_id], start_ts, end_ts, ...]
# REPLACE service_id with actual value from GetAppointmentServiceDefinition response
PLACEHOLDER_SERVICE_ID = "REPLACE_ME"

post_body_placeholder = json.dumps([
    PLACEHOLDER_SERVICE_ID,
    str(now_ts),
    str(week_ts),
    None, None, None, None, None, None, 1
])

params = urllib.parse.urlencode({"$httpHeaders": HTTP_HEADERS_ANON})
url = f"{BASE_URL}?{params}"

print("=" * 55)
print("Testing ListAvailableSlots without Authorization header")
print("=" * 55)
print(f"URL: {url[:80]}...")
print(f"Sending POST to: calendar-pa.clients6.google.com")
print()

try:
    req = urllib.request.Request(
        url,
        data=post_body_placeholder.encode("utf-8"),
        headers={
            "Content-Type": "application/x-www-form-urlencoded;charset=UTF-8",
            "Origin": "https://calendar.google.com",
            "Referer": "https://calendar.google.com/",
            "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/147.0.0.0",
        },
        method="POST"
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        status = resp.status
        body = resp.read()
    print(f"HTTP Status : {status}")
    print(f"Body length : {len(body)} bytes")
    print(f"Body preview: {body[:200]}")
except urllib.error.HTTPError as e:
    print(f"HTTP Error  : {e.code} {e.reason}")
    body = e.read()
    print(f"Response    : {body[:300]}")
    if e.code == 401:
        print("\n=> Authorization IS required (can't call anonymously)")
    elif e.code == 400:
        print("\n=> Bad request — API key may be OK but body format is wrong")
        print("   (This is actually good news — auth might not be needed!)")
    elif e.code == 403:
        print("\n=> Forbidden — API key likely required / invalid")
except Exception as e:
    print(f"Error: {e}")

print()
print("Next step: share the Payload tab from DevTools so we can fill in")
print("the correct POST body format (service ID per room).")
