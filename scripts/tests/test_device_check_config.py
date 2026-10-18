#!/usr/bin/env python3
"""The device checker builds current atomic commands without touching a device."""
import copy
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("device_check", ROOT / "scripts/device_check.py")
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class DeviceCheckConfig(unittest.TestCase):
    def test_command_preserves_other_sections_and_absent_secrets(self):
        wire = {"revision": 17, "desired": {
            "settings": {"hostname": "saved", "browser_debug": False,
                         "wifi_password_set": True, "wifi_ssid": {"base64": "/w=="}},
            "competitions": [{"id": 5, "club": {"team": 40, "group": 1}}],
            "screens": {"frame": False}}}
        device = checker.Device("unused.invalid")
        calls = []

        def call(method, path, body=None, token=None):
            calls.append((method, path, body, token))
            if method == "GET":
                self.assertEqual(path, "config")
                return 200, None, b"", copy.deepcopy(wire)
            self.assertEqual((method, path), ("PUT", "config"))
            return 200, None, b"", {"revision": 18, "apply_id": body["apply_id"]}

        device.call = call
        device.save("test-session", {"browser_debug": True})
        command = calls[1][2]
        self.assertEqual(command["expected_revision"], 17)
        self.assertRegex(command["apply_id"], r"^[0-9a-f]{16}$")
        self.assertNotEqual(command["apply_id"], "0" * 16)
        self.assertEqual(command["desired"]["screens"], wire["desired"]["screens"])
        self.assertEqual(command["desired"]["competitions"], wire["desired"]["competitions"])
        self.assertEqual(command["desired"]["settings"]["wifi_ssid"], {"base64": "/w=="})
        self.assertNotIn("wifi_password", command["desired"]["settings"])
        self.assertNotIn("wifi_password_set", command["desired"]["settings"])
        self.assertNotIn("adopted_names", command)
        self.assertFalse(wire["desired"]["settings"]["browser_debug"])

    def test_ambiguous_ack_blocks_following_mutations(self):
        class Response:
            status = 200
            def read(self): return b'{"ok":true}'
        class Connection:
            sent = 0
            def request(self, *args, **kwargs): self.sent += 1
            def getresponse(self): return Response()
            def close(self): pass
        device = checker.Device("unused.invalid")
        wire = {"revision": 1, "desired": {"settings": {}, "competitions": [], "screens": {}}}
        device.config = lambda: wire
        connection = Connection()
        device.connection = lambda: connection
        with self.assertRaisesRegex(RuntimeError, "acknowledgement"):
            device.save("token", {"browser_debug": True})
        with self.assertRaisesRegex(RuntimeError, "unknown"):
            device.call("POST", "reboot", {}, "token")
        self.assertEqual(connection.sent, 1)

    def test_obsolete_routes_are_absent(self):
        web = (ROOT / "components/web/web_service.cpp").read_text()
        for path in ("settings", "competitions", "screen-settings"):
            self.assertNotIn(f'{{"/api/v1/{path}",', web)

    def test_heap_load_rejects_client_errors_and_never_borrows_selection(self):
        """Actual checker previously accepted400s as successful load traffic."""
        class Event:
            checks = iter((False, True, False, True))
            def is_set(self): return next(self.checks)
            def set(self): pass
        class Thread:
            def __init__(self, target): self.target = target
            def start(self): self.target()
            def join(self): pass
        class Connection:
            def close(self): pass
        device = checker.Device("unused.invalid")
        device.connection = Connection
        calls, reports = [], []
        status = {"heap_free": 100000, "heap_largest": 90000,
                  "uptime_s": 100, "reset_reason": "test"}
        def call(method, path, **kwargs):
            self.assertEqual(method, "GET")
            calls.append(path)
            return (400 if path == "config" else 200), None, b"", status
        device.call = call
        with (patch.object(checker.threading, "Event", Event),
              patch.object(checker.threading, "Thread", Thread),
              patch.object(checker.time, "time", side_effect=(0, 21)),
              patch.object(checker, "report", side_effect=lambda ok, name, detail="": reports.append((ok, name, detail))),
              patch.object(checker, "info"), patch.object(checker, "check_reserves")):
            checker.check_heap(device, "test-session")
        self.assertNotIn("selection", calls)
        self.assertEqual(calls.count("demo"), 2)
        load = next(row for row in reports if "2 clients plus a sampler" in row[1])
        self.assertFalse(load[0], "HTTP400 is a failed load request")
        self.assertIn("2 errors", load[2])
        self.assertIn("HTTP 400", load[2])


if __name__ == "__main__":
    unittest.main()
