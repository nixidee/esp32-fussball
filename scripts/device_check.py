#!/usr/bin/env python3
"""Black-box checks of the device HTTP API against a running device.

    python3 scripts/device_check.py HOST [quick]               ~1 minute
    python3 scripts/device_check.py HOST sessions [--minutes N] 16.5 minutes

Standard library only. Uses the public wire format: POST /session for a token,
header X-CSRF-Token, JSON bodies, {"error","code"[,"field"]} for errors.

'quick' covers: unauthenticated writes (W03), keep-alive after an error,
field-level errors, session slots (LRU, logout), the debug WebSocket (refused
without token, with a foreign origin and with browser_debug off; one stream at a
time; ends with browser_debug off and with its session), image upload/replace/
delete with the cache header and revision, and heap and stack reserve under load
(read from /status: heap_min, tasks[].stack_min_free).

It changes device state and restores it: browser_debug is set and put back,
one test image is stored and deleted again, harmless saves keep the stored
value. It never reboots, resets, flashes or opens the serial port.
"""

import argparse
import base64
import http.client
import json
import os
import socket
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
API = "/api/v1/"
# Limits of the load check. Chosen here, not taken from a requirement: a task
# that comes closer than STACK_RESERVE_BYTES to overflowing, or a heap that
# drops below HEAP_RESERVE_BYTES since boot (a provider TLS handshake alone
# takes about 40 KB), fails the check.
STACK_RESERVE_BYTES = 256
HEAP_RESERVE_BYTES = 20000
failures = 0


def report(ok, name, detail=""):
    global failures
    if not ok:
        failures += 1
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  [{detail}]" if detail else ""))


def info(name, detail=""):
    print(f"INFO  {name}" + (f"  [{detail}]" if detail else ""))


class Device:
    def __init__(self, host):
        self.host = host

    def connection(self):
        return http.client.HTTPConnection(self.host, 80, timeout=15)

    def call(self, method, path, body=None, token=None, headers=None, conn=None):
        """One request. Returns (status, headers, raw, json-or-None)."""
        own = conn is None
        conn = conn or self.connection()
        h = dict(headers or {})
        if token:
            h["X-CSRF-Token"] = token
        if isinstance(body, (dict, list)):
            body = json.dumps(body)
        if isinstance(body, str):
            body = body.encode()
        if body is not None and "Content-Type" not in h:
            h["Content-Type"] = "application/json"
        conn.request(method, API + path, body=body, headers=h)
        response = conn.getresponse()
        raw = response.read()
        if own:
            conn.close()
        try:
            parsed = json.loads(raw)
        except ValueError:
            parsed = None
        return response.status, response, raw, parsed

    def login(self):
        status, _, _, data = self.call("POST", "session", {})
        if status != 200 or not data or "token" not in data:
            raise SystemExit(f"login failed: HTTP {status} {data}")
        return data["token"]

    def settings(self):
        return self.call("GET", "settings")[3]

    def save(self, token, change):
        return self.call("PUT", "settings", change, token)


def unchanged_field(device):
    """A setting to write back with its own value: a save without any change."""
    current = device.settings()
    for key in ("debug_status_interval_s", "brightness"):
        if key in current:
            return {key: current[key]}
    key = next(k for k, v in current.items() if isinstance(v, int))
    return {key: current[key]}


# ---- 1. W03: unauthenticated writes --------------------------------------
def check_unauthenticated(device):
    base = device.settings()
    raw = json.dumps(base)
    for size in (2, len(raw), 2291, 8192, 30000):
        body = "{}" if size == 2 else raw.ljust(size)
        name = f"W03 unauthenticated PUT, {size} B body"
        conn = device.connection()
        try:
            status, response, _, data = device.call("PUT", "settings", body, conn=conn)
            kept = False
            if not response.will_close:
                kept = device.call("GET", "status", conn=conn)[0] == 200
            report(
                status == 401 and data and data.get("code") == "session" and kept,
                name,
                f"HTTP {status}, code {data and data.get('code')}, "
                f"connection {'reused' if kept else 'closed'}",
            )
        except (OSError, http.client.HTTPException) as err:
            report(False, name, f"{type(err).__name__}: {err}")
        finally:
            conn.close()
    # Above the 32 KB drain limit the connection may be closed; a reset is a
    # known limit of that case, so it is reported but not counted.
    conn = device.connection()
    try:
        status = device.call("PUT", "settings", raw.ljust(40000), conn=conn)[0]
        info("unauthenticated PUT, 40000 B body (above the drain limit)", f"HTTP {status}")
    except (OSError, http.client.HTTPException) as err:
        info("unauthenticated PUT, 40000 B body (above the drain limit)", type(err).__name__)
    finally:
        conn.close()


# ---- 2. keep-alive after an error ---------------------------------------
def check_keepalive(device, token):
    conn = device.connection()
    try:
        status, response, _, data = device.call(
            "PUT", "settings", {"brightness": 999}, token, conn=conn
        )
        port = conn.sock.getsockname()[1] if conn.sock else None
        again = device.call("GET", "status", conn=conn)
        same = conn.sock is not None and conn.sock.getsockname()[1] == port
        report(
            status == 400 and not response.will_close and again[0] == 200 and same,
            "keep-alive after a 400",
            f"first HTTP {status}, Connection: {response.getheader('Connection')}, "
            f"second HTTP {again[0]}, same socket {same}",
        )
    finally:
        conn.close()


# ---- 3. every value error names its field ----------------------------
FIELD_CASES = [
    # method, path, body, status, code, field   (code None = not compared)
    ("PUT", "settings", {"brightness": 999}, 400, "range", "brightness"),
    ("PUT", "settings", {"nonsense": 1}, 400, "unknown_key", "nonsense"),
    ("PUT", "settings", {"brightness": "x"}, 400, "type", "brightness"),
    ("POST", "data/select", {"teams": True}, 400, "required", "provider"),
    ("POST", "data/select", {"provider": 9, "teams": True}, 400, "range", "provider"),
    ("POST", "data/select", {"provider": 0}, 400, "required", "teams"),
    ("POST", "data/select", {"provider": 0, "teams": "x"}, 400, "type", "teams"),
    ("POST", "data/select", {"provider": 0, "teams": True}, 400, "required", "competition"),
    ("POST", "data/select", {"provider": 0, "teams": False, "offset": "x"}, 400, "type", "offset"),
    ("POST", "data/select", {"provider": 0, "teams": False, "search": 5}, 400, "type", "search"),
    ("POST", "data/fixtures", {"competition": 9}, 400, "range", "competition"),
    ("POST", "data/fixtures", {}, 400, "required", "competition"),
    ("POST", "data/fixtures", {"competition": "a"}, 400, "type", "competition"),
    ("POST", "display", {"action": "x"}, 400, "invalid", "action"),
    ("POST", "display", {}, 400, "required", "action"),
    ("POST", "display", {"action": 1}, 400, "type", "action"),
    ("POST", "images/delete", {"id": 99}, 400, "range", "id"),
    ("POST", "images/delete", {}, 400, "required", "id"),
    ("POST", "images/delete", {"id": "a"}, 400, "type", "id"),
    ("POST", "reset", {"confirm": "no"}, 400, "invalid", "confirm"),
    ("POST", "reset", {}, 400, "invalid", "confirm"),
    ("GET", "fixtures", None, 400, "required", "competition"),
    ("GET", "fixtures?competition=9&source=primary", None, 400, "range", "competition"),
    ("GET", "fixtures?competition=0", None, 400, "required", "source"),
    ("GET", "fixtures?competition=0&source=x", None, 400, "invalid", "source"),
    ("GET", "images/file", None, 400, "required", "id"),
    ("GET", "images/file?id=99", None, 400, "range", "id"),
    ("GET", "images/file?id=abc", None, 400, "range", "id"),
    ("GET", "images/file?stock=x", None, 400, "invalid", "stock"),
]


def check_fields(device, token):
    for method, path, body, status, code, field in FIELD_CASES:
        label = f"{method} {path} {json.dumps(body) if body is not None else ''}".strip()
        got, _, _, data = device.call(method, path, body, token)
        data = data or {}
        ok = (
            got == status
            and data.get("field") == field
            and (code is None or data.get("code") == code)
            and bool(data.get("error"))
        )
        report(ok, label, f"HTTP {got}, code {data.get('code')}, field {data.get('field')}")
    # Answers that are not value errors keep their own status.
    for label, call, status, code in (
        ("no token", lambda: device.call("POST", "data/select", {"teams": True}), 401, "session"),
        ("unknown route", lambda: device.call("GET", "nothing"), 404, "unknown_route"),
        ("wrong method", lambda: device.call("DELETE", "settings"), 405, None),
        ("missing image", lambda: device.call("GET", "images/file?id=10"), 404, "not_found"),
    ):
        got, _, _, data = call()
        data = data or {}
        report(
            got == status and (code is None or data.get("code") == code),
            f"status {status} for {label}",
            f"HTTP {got}, code {data.get('code')}",
        )
    # Bodies that are no value: no field is expected, only a readable error.
    for label, body, headers in (
        ("no body", None, None),
        ("invalid JSON", "{nope", None),
        ("wrong content type", "{}", {"Content-Type": "text/plain"}),
    ):
        got, _, _, data = device.call("PUT", "settings", body, token, headers)
        data = data or {}
        report(
            got in (400, 415) and bool(data.get("error")) and bool(data.get("code")),
            f"PUT settings, {label}",
            f"HTTP {got}, code {data.get('code')}, field {data.get('field')}",
        )


# ---- 4. session slots ------------------------------------------------
def check_slots(device):
    tokens = [device.login() for _ in range(4)]
    save = unchanged_field(device)
    # tokens[0] is used again, so tokens[1] is the least recently used one.
    report(device.save(tokens[0], save)[0] == 200, "slot 1 usable after 3 more logins")
    fifth = device.login()
    states = [device.save(t, save)[0] for t in tokens + [fifth]]
    report(
        states[1] == 401 and states[0] == states[2] == states[3] == states[4] == 200,
        "fifth login evicts the least recently used session",
        f"HTTP {states}",
    )
    status, _, _, data = device.call("POST", "logout", {}, fifth)
    after = device.save(fifth, save)[0]
    report(status == 200 and after == 401, "logout ends the session", f"logout {status}, then {after}")


# ---- 5. debug WebSocket ------------------------------------------------
def ws_open(host, token, origin=True):
    """Opens the debug socket. Returns (socket, status line, error JSON or None):
    101 means the stream is open, anything else is the refusal httpd sent."""
    sock = socket.create_connection((host, 80), timeout=5)
    query = f"?token={token}" if token else ""
    sock.sendall(
        (
            f"GET {API}debug{query} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {base64.b64encode(os.urandom(16)).decode()}\r\n"
            f"Sec-WebSocket-Version: 13\r\n"
            + (f"Origin: http://{host}\r\n" if origin is True else f"Origin: {origin}\r\n" if origin else "")
            + "\r\n"
        ).encode()
    )
    head = b""
    while b"\r\n\r\n" not in head:
        chunk = sock.recv(1)
        if not chunk:
            return sock, head.split(b"\r\n")[0].decode(errors="replace") or "closed", None
        head += chunk
    lines = head.decode(errors="replace").split("\r\n")
    if " 101 " in lines[0]:
        return sock, lines[0], None
    length = next((int(l.split(":")[1]) for l in lines if l.lower().startswith("content-length:")), 0)
    body = b""
    while len(body) < length:
        chunk = sock.recv(length - len(body))
        if not chunk:
            break
        body += chunk
    try:
        return sock, lines[0], json.loads(body)
    except ValueError:
        return sock, lines[0], None


def ws_frame(sock, timeout):
    """('text', str) | ('close', None) | ('eof', None) | ('timeout', None)."""
    sock.settimeout(timeout)

    def take(count):
        data = b""
        while len(data) < count:
            chunk = sock.recv(count - len(data))
            if not chunk:
                raise EOFError
            data += chunk
        return data

    try:
        first, second = take(2)
        length = second & 0x7F
        if length == 126:
            length = int.from_bytes(take(2), "big")
        elif length == 127:
            length = int.from_bytes(take(8), "big")
        payload = take(length)
    except socket.timeout:
        return "timeout", None
    except (EOFError, OSError):
        return "eof", None
    opcode = first & 0x0F
    return ("text", payload.decode()) if opcode == 1 else ("close", None) if opcode == 8 else ("other", None)


def wait_for_end(sock, seconds):
    """Seconds until the stream ends (EOF or close frame); None if it does not."""
    start = time.time()
    while time.time() - start < seconds:
        kind, _ = ws_frame(sock, 1.5)
        if kind in ("eof", "close"):
            return time.time() - start
    return None


def refused(host, token, status, code, origin=True):
    """Opens the socket and returns (ok, detail): refused with 'status' and 'code'."""
    sock, line, data = ws_open(host, token, origin)
    sock.close()
    return f" {status} " in line and (data or {}).get("code") == code, f"{line}, {data}"


def check_websocket(device, token):
    original = device.settings().get("browser_debug")
    try:
        device.save(token, {"browser_debug": False})
        ok, detail = refused(device.host, token, 403, "debug_disabled")
        report(ok, "debug socket refused while browser_debug is off", detail)

        device.save(token, {"browser_debug": True})
        ok, detail = refused(device.host, None, 401, "session")
        report(ok, "debug socket refused without a token", detail)
        ok, detail = refused(device.host, "x" * 32, 401, "session")
        report(ok, "debug socket refused with an unknown token", detail)
        ok, detail = refused(device.host, token, 403, "origin", origin="http://evil.example")
        report(ok, "debug socket refused for a foreign origin", detail)

        sock, line, _ = ws_open(device.host, token)
        frames = []
        for _ in range(3):
            kind, text = ws_frame(sock, 3)
            if kind == "text":
                frames.append(json.loads(text))
        report(
            " 101 " in line and len(frames) >= 2 and all("heap" in f and "ms" in f for f in frames),
            "debug socket with a token streams records",
            f"{line}; {len(frames)} records, heap {[f.get('heap') for f in frames]}",
        )
        ok, detail = refused(device.host, token, 409, "busy")
        report(ok, "a second debug socket is refused while one is open", detail)

        device.save(token, {"browser_debug": False})
        ended = wait_for_end(sock, 5)
        report(ended is not None and ended <= 3.5, "stream ends when browser_debug is switched off",
               f"{ended and round(ended, 1)} s")
        sock.close()

        device.save(token, {"browser_debug": True})
        own = device.login()
        sock, line, _ = ws_open(device.host, own)
        kind, _ = ws_frame(sock, 3)
        device.call("POST", "logout", {}, own)
        ended = wait_for_end(sock, 5)
        report(" 101 " in line and kind == "text" and ended is not None and ended <= 3.5,
               "stream ends when its session is closed", f"first {kind}, ended after {ended and round(ended, 1)} s")
        sock.close()
    finally:
        if original is not None:
            device.save(token, {"browser_debug": original})


# ---- 6. image upload, cache header, revision --------------------------
def while_busy(call, tries=20):
    """Repeats a call the device answers with 409 busy (a provider fetch holds
    the one heavy-operation token); this is how a client has to behave."""
    for _ in range(tries):
        result = call()
        if not (result[0] == 409 and (result[3] or {}).get("code") == "busy"):
            return result
        time.sleep(2)
    return result


def upload(device, token, data, image_id=None):
    headers = {"Content-Type": "image/jpeg"}
    if image_id:
        headers["X-Image-Id"] = str(image_id)
    return while_busy(lambda: device.call("POST", "images/upload", data, token, headers))


def check_images(device, token):
    listing = device.call("GET", "images")[3]
    if listing["count"]:
        info("images: library not empty, test skipped to keep your images", f"{listing['count']} stored")
        return
    first = (ROOT / "data" / "default_crest.jpg").read_bytes()
    second = (ROOT / "data" / "default_background.jpg").read_bytes()
    stored = None
    try:
        status, _, _, data = upload(device, token, first)
        stored = data and data.get("id")
        report(status == 200 and stored, "image upload", f"HTTP {status}, {data}")
        if not stored:
            return

        def revision():
            return next(i for i in device.call("GET", "images")[3]["items"] if i["id"] == stored)["revision"]

        r1 = revision()
        status, response, raw, _ = device.call("GET", f"images/file?id={stored}&r={r1}")
        report(
            status == 200 and raw == first
            and response.getheader("Cache-Control") == "public, max-age=31536000, immutable"
            and response.getheader("Content-Type") == "image/jpeg",
            "file with ?r= is served as cacheable for good",
            f"{response.getheader('Cache-Control')}, {len(raw)} B",
        )
        status, response, _, _ = device.call("GET", f"images/file?id={stored}")
        report(status == 200 and response.getheader("Cache-Control") == "no-cache",
               "file without ?r= is revalidated", response.getheader("Cache-Control"))
        status, _, _, data = upload(device, token, second, stored)
        r2 = revision()
        status2, _, raw, _ = device.call("GET", f"images/file?id={stored}&r={r2}")
        report(status == 200 and r2 != r1 and raw == second, "replace changes revision and content",
               f"upload HTTP {status} {data}, revision {r1} -> {r2}, content {'new' if raw == second else 'old' if raw == first else '?'}")
        status, _, _, data = while_busy(lambda: device.call("POST", "images/delete", {"id": stored}, token))
        gone = device.call("GET", f"images/file?id={stored}&r={r2}")
        stored = None if status == 200 else stored
        report(status == 200 and gone[0] == 404 and gone[3].get("code") == "not_found",
               "delete removes the image", f"HTTP {status}, then {gone[0]}")
    finally:
        if stored:
            status = while_busy(lambda: device.call("POST", "images/delete", {"id": stored}, token))[0]
            info("cleanup: test image deleted" if status == 200 else "cleanup FAILED, delete image 1 by hand", f"HTTP {status}")


# ---- 7. heap and stack reserve under load --------------------------------
def check_heap(device, token):
    def status():
        return device.call("GET", "status")[3]

    before = status()
    stop = threading.Event()
    errors = []

    def load():
        conn = device.connection()  # one kept connection per client, like a browser
        while not stop.is_set():
            for path in ("status", "settings", "images", "schema", "selection"):
                try:
                    if device.call("GET", path, conn=conn)[0] >= 500:
                        errors.append(path)
                except (OSError, http.client.HTTPException) as err:
                    errors.append(f"{path}: {type(err).__name__}")
                    conn.close()
                    conn = device.connection()
                    time.sleep(1)
        conn.close()

    threads = [threading.Thread(target=load) for _ in range(2)]
    for t in threads:
        t.start()
    low_free = before["heap_free"]
    low_largest = before["heap_largest"]
    end = time.time() + 20
    while time.time() < end:
        try:
            now = status()
            low_free = min(low_free, now["heap_free"])
            low_largest = min(low_largest, now["heap_largest"])
        except (OSError, http.client.HTTPException, ValueError, TypeError):
            errors.append("sampler")
        time.sleep(0.3)
    stop.set()
    for t in threads:
        t.join()
    after = status()
    rebooted = after["uptime_s"] < before["uptime_s"]
    report(not rebooted, "device did not restart during the load",
           f"uptime {before['uptime_s']} -> {after['uptime_s']} s, last reset: {after['reset_reason']}")
    report(not errors, "2 clients plus a sampler for 20 s without a failed request", f"{len(errors)} errors {errors[:3]}")
    if "heap_min" not in after or "tasks" not in after:
        info("heap_min and tasks are missing in /status: firmware without the load-check fields")
        return
    info("heap now", f"free {before['heap_free']} -> lowest sampled {low_free} -> {after['heap_free']} B; "
         f"largest block lowest {low_largest} B")
    report(after["heap_min"] >= HEAP_RESERVE_BYTES, f"lowest free heap since boot is at least {HEAP_RESERVE_BYTES} B",
           f"{after['heap_min']} B")
    tasks = sorted(after["tasks"], key=lambda t: t["stack_min_free"])
    tight = [t for t in tasks if t["stack_min_free"] < STACK_RESERVE_BYTES]
    report(bool(tasks) and not tight, f"every task keeps at least {STACK_RESERVE_BYTES} B of stack free",
           f"{len(tasks)} tasks; lowest " + ", ".join(
               f"{t['name'] or '#' + str(t['id'])} {t['stack_min_free']} B" for t in tasks[:4]))


# ---- long run: parallel sessions over more than 15 minutes -------------
def run_sessions(device, minutes):
    save = unchanged_field(device)
    a, b, idle = device.login(), device.login(), device.login()
    start = time.time()
    step = 0
    print(f"sessions A and B save alternately every 60 s for {minutes} min; C stays idle")
    while time.time() - start < minutes * 60:
        name, token = ("A", a) if step % 2 == 0 else ("B", b)
        status = device.save(token, save)[0]
        elapsed = int(time.time() - start)
        report(status == 200, f"t={elapsed // 60:2d}:{elapsed % 60:02d}  session {name} saves", f"HTTP {status}")
        step += 1
        time.sleep(60)
    final = [device.save(t, save)[0:4:3] for t in (a, b, idle)]
    report(final[0][0] == 200 and final[1][0] == 200, "A and B still valid after the whole run", f"{final[0][0]}, {final[1][0]}")
    code = (final[2][1] or {}).get("code")
    report(final[2][0] == 401 and code == "session", "idle session C expired", f"HTTP {final[2][0]}, code {code}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("host")
    parser.add_argument("mode", nargs="?", default="quick", choices=("quick", "sessions"))
    parser.add_argument("--minutes", type=float, default=16.5)
    args = parser.parse_args()
    device = Device(args.host)
    if args.mode == "sessions":
        run_sessions(device, args.minutes)
    else:
        token = device.login()
        check_unauthenticated(device)
        check_keepalive(device, token)
        check_fields(device, token)
        check_slots(device)
        token = device.login()
        check_websocket(device, token)
        check_images(device, token)
        check_heap(device, token)
    print(f"\n{'ALL PASSED' if not failures else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
