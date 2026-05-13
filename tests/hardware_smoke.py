#!/usr/bin/env python3
"""Flash-and-boot smoke test for the GDO blaQ HomeKit firmware.

Run from a shell where ESP-IDF is exported. The script intentionally keeps
hardware actions behind explicit flags so it can also be used as a monitor-only
boot smoke test.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import deque
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - exercised on machines without pyserial
    serial = None
    list_ports = None


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_APP_READY_RE = re.compile(r"\btest_main:\s+GDO started!")
DEFAULT_HEALTH_READY_RE = re.compile(r"\bapp_health:\s+Reset reason:")
DEFAULT_AP_READY_RE = re.compile(r"\bnvs_wifi_connect:\s+wifi_init_softap finished\. SSID:konnected-blaq-hk\b")
DEFAULT_AP_MODE_RE = re.compile(r"\bwifi:\s+Running in AP mode\b")
DEFAULT_STA_READY_RE = re.compile(r"\b(?:wifi:\s+Connected in STA mode|nvs_wifi_connect:\s+got ip:)")
DEFAULT_HTTP_READY_RE = re.compile(r"\bnvs_wifi_connect_http_server:\s+Registering URI handlers\b")
DEFAULT_WIFI_NOT_CONFIGURED_RE = re.compile(r"\bwifi:\s+WiFi not configured\b")

FATAL_RES = [
    re.compile(pattern)
    for pattern in (
        r"Guru Meditation Error",
        r"panic'ed",
        r"assert failed:",
        r"\babort\(\)",
        r"\bBacktrace:",
        r"Brownout detector was triggered",
        r"\bWDT reset\b",
    )
]


class SmokeFailure(RuntimeError):
    pass


class SmokeState:
    def __init__(self) -> None:
        self.app_ready = False
        self.health_ready = False
        self.ap_ready = False
        self.sta_ready = False
        self.http_ready = False
        self.wifi_not_configured = False
        self.last_lines: deque[str] = deque(maxlen=80)

    def feed(self, line: str) -> None:
        self.last_lines.append(line)
        if DEFAULT_APP_READY_RE.search(line):
            self.app_ready = True
        if DEFAULT_HEALTH_READY_RE.search(line):
            self.health_ready = True
        if DEFAULT_AP_READY_RE.search(line) or DEFAULT_AP_MODE_RE.search(line):
            self.ap_ready = True
        if DEFAULT_STA_READY_RE.search(line):
            self.sta_ready = True
        if DEFAULT_HTTP_READY_RE.search(line):
            self.http_ready = True
        if DEFAULT_WIFI_NOT_CONFIGURED_RE.search(line):
            self.wifi_not_configured = True

        for fatal_re in FATAL_RES:
            if fatal_re.search(line):
                raise SmokeFailure(f"fatal boot log matched: {fatal_re.pattern}: {line}")

    def missing(self, expect_wifi: str) -> list[str]:
        missing: list[str] = []
        if not self.app_ready:
            missing.append("app boot marker: test_main: GDO started!")
        if not self.health_ready:
            missing.append("health supervisor startup marker: app_health: Reset reason:")

        if expect_wifi == "ap":
            if not self.ap_ready:
                missing.append("AP mode marker for SSID konnected-blaq-hk")
            if not self.http_ready:
                missing.append("provisioning HTTP handler registration")
        elif expect_wifi == "sta":
            if not self.sta_ready:
                missing.append("STA connected marker or got ip log")
            if not self.http_ready:
                missing.append("Wi-Fi configuration HTTP handler registration")
        elif expect_wifi == "any":
            if not (self.ap_ready or self.sta_ready):
                missing.append("AP provisioning or STA connected Wi-Fi startup marker")
            elif not self.http_ready:
                missing.append("Wi-Fi configuration HTTP handler registration")

        return missing

    def passed(self, expect_wifi: str) -> bool:
        return not self.missing(expect_wifi)


def idf_py() -> list[str]:
    resolved = shutil.which("idf.py")
    if resolved:
        return [resolved]

    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        candidate = Path(idf_path) / "tools" / "idf.py"
        if candidate.exists():
            return [sys.executable, str(candidate)]

    raise SmokeFailure("idf.py not found. Source ESP-IDF first, for example: . $IDF_PATH/export.sh")


def require_pyserial() -> None:
    if serial is None or list_ports is None:
        raise SmokeFailure("pyserial is not available. Source ESP-IDF or install pyserial in this Python environment.")


def run(cmd: list[str], cwd: Path = REPO_ROOT) -> None:
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def known_serial_ports() -> list[str]:
    require_pyserial()
    assert list_ports is not None
    ports = []
    for port in list_ports.comports():
        ports.append(f"{port.device}\t{port.description}\t{port.hwid}")
    return ports


def autodetect_port() -> str:
    require_pyserial()
    assert list_ports is not None
    candidates = []
    for port in list_ports.comports():
        device = port.device
        text = f"{port.device} {port.description} {port.hwid}".lower()
        likely_name = (
            device.startswith("/dev/cu.usb")
            or device.startswith("/dev/ttyUSB")
            or device.startswith("/dev/ttyACM")
        )
        likely_description = any(token in text for token in ("esp32", "jtag", "cp210", "usb serial", "wch", "ch340"))
        if likely_name or likely_description:
            candidates.append(port.device)

    if len(candidates) == 1:
        return candidates[0]

    formatted = "\n".join(known_serial_ports()) or "no serial ports found"
    if not candidates:
        raise SmokeFailure(f"could not auto-detect an ESP serial port. Available ports:\n{formatted}")
    raise SmokeFailure(f"multiple possible ESP serial ports: {', '.join(candidates)}. Available ports:\n{formatted}")


def resolve_port(args: argparse.Namespace) -> str:
    if args.port:
        return args.port
    if os.environ.get("ESPPORT"):
        return os.environ["ESPPORT"]
    if args.auto_port:
        return autodetect_port()
    raise SmokeFailure("serial port is required. Pass --port, set ESPPORT, or use --auto-port.")


def maybe_build(args: argparse.Namespace) -> None:
    if args.build:
        run(idf_py() + ["build"])


def maybe_flash(args: argparse.Namespace, port: str) -> None:
    if not args.flash:
        return

    actions = ["erase-flash", "flash"] if args.erase_flash else ["flash"]
    run(idf_py() + ["-p", port, "-b", str(args.flash_baud), *actions])


def pulse_reset(ser: "serial.Serial") -> None:
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    time.sleep(0.1)


def monitor_boot(args: argparse.Namespace, port: str) -> SmokeState:
    require_pyserial()
    assert serial is not None
    state = SmokeState()
    deadline = time.monotonic() + args.timeout
    post_ready_deadline: float | None = None
    partial = b""

    print(f"Monitoring {port} at {args.monitor_baud} baud for up to {args.timeout}s...", flush=True)
    with serial.Serial(port, baudrate=args.monitor_baud, timeout=0.2) as ser:
        ser.reset_input_buffer()
        if args.reset or args.flash:
            pulse_reset(ser)
        while time.monotonic() < (post_ready_deadline or deadline):
            data = ser.read(ser.in_waiting or 1)
            if not data:
                continue

            partial += data
            while b"\n" in partial:
                raw_line, partial = partial.split(b"\n", 1)
                line = raw_line.decode("utf-8", errors="replace").rstrip("\r")
                if args.print_log:
                    print(line)
                state.feed(line)
                if state.passed(args.expect_wifi) and post_ready_deadline is None:
                    if args.post_ready_seconds <= 0:
                        return state
                    post_ready_deadline = time.monotonic() + args.post_ready_seconds
                    print(
                        f"Boot smoke markers reached; monitoring for {args.post_ready_seconds:.1f}s more...",
                        flush=True,
                    )

    if post_ready_deadline is not None:
        return state

    missing = "\n - ".join(state.missing(args.expect_wifi))
    tail = "\n".join(state.last_lines)
    raise SmokeFailure(f"timed out waiting for boot smoke markers:\n - {missing}\n\nLast serial lines:\n{tail}")


def probe_http(url: str, timeout: float) -> None:
    print(f"Probing {url}...", flush=True)
    request = urllib.request.Request(url, headers={"User-Agent": "gdo-blaq-homekit-smoke/1.0"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        body = response.read(8192)
        status = getattr(response, "status", response.getcode())

    if status != 200:
        raise SmokeFailure(f"HTTP probe returned status {status}")
    expected = (b"GDO blaQ HomeKit", b"Device Status", b"Wi-Fi Settings")
    if not any(marker in body for marker in expected):
        raise SmokeFailure("HTTP probe succeeded but response did not look like the device status page")


def api_auth_headers(admin_pin: str | None) -> dict[str, str]:
    return {"X-Admin-PIN": admin_pin} if admin_pin else {}


def probe_status_api(base_url: str, timeout: float, admin_pin: str | None) -> None:
    status_url = urllib.parse.urljoin(base_url, "/api/status")
    print(f"Probing {status_url}...", flush=True)
    headers = {"User-Agent": "gdo-blaq-homekit-smoke/1.0"}
    headers.update(api_auth_headers(admin_pin))
    request = urllib.request.Request(status_url, headers=headers)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        body = response.read(16384)
        status = getattr(response, "status", response.getcode())

    if status != 200:
        raise SmokeFailure(f"status API probe returned status {status}")

    try:
        data = json.loads(body.decode("utf-8"))
    except json.JSONDecodeError as exc:
        raise SmokeFailure(f"status API returned invalid JSON: {exc}") from exc

    for key in ("app", "gdo", "wifi", "heap"):
        if key not in data:
            raise SmokeFailure(f"status API response missing {key!r}")


def http_json(method: str, url: str, timeout: float, payload: dict | None = None, headers: dict | None = None) -> tuple[int, dict]:
    data = None
    request_headers = {"User-Agent": "gdo-blaq-homekit-smoke/1.0", "Accept": "application/json"}
    if headers:
        request_headers.update(headers)
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        request_headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=data, headers=request_headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read(16384)
            status = getattr(response, "status", response.getcode())
    except urllib.error.HTTPError as exc:
        body = exc.read(16384)
        status = exc.code

    try:
        parsed = json.loads(body.decode("utf-8")) if body else {}
    except json.JSONDecodeError as exc:
        raise SmokeFailure(f"{method} {url} returned invalid JSON: {exc}") from exc
    return status, parsed


def probe_management_apis(base_url: str, timeout: float, admin_pin: str | None) -> None:
    endpoints = {
        "events": "/api/events?limit=5",
        "settings": "/api/settings",
        "homekit setup": "/api/homekit/setup",
    }
    for name, path in endpoints.items():
        url = urllib.parse.urljoin(base_url, path)
        print(f"Probing {url}...", flush=True)
        status, data = http_json("GET", url, timeout, headers=api_auth_headers(admin_pin))
        if status != 200:
            raise SmokeFailure(f"{name} API returned status {status}")
        if name == "events" and "events" not in data:
            raise SmokeFailure("events API missing events list")
        if name == "settings" and "settings" not in data:
            raise SmokeFailure("settings API missing settings object")
        if name == "homekit setup" and "setup_available" not in data:
            raise SmokeFailure("homekit setup API missing setup_available")

    clear_url = urllib.parse.urljoin(base_url, "/api/events/clear")
    print(f"Checking admin rejection on {clear_url}...", flush=True)
    status, _ = http_json("POST", clear_url, timeout, payload={})
    if status != 403:
        raise SmokeFailure(f"admin-protected events clear returned {status} without PIN; expected 403")

    position_url = urllib.parse.urljoin(base_url, "/api/gdo/position")
    status, _ = http_json("POST", position_url, timeout, payload={"target_percent": 50})
    if status != 403:
        raise SmokeFailure(f"admin-protected position command returned {status} without PIN; expected 403")

    if not admin_pin:
        return

    settings_url = urllib.parse.urljoin(base_url, "/api/settings")
    headers = {"X-Admin-PIN": admin_pin}
    status, settings = http_json("GET", settings_url, timeout, headers=headers)
    if status != 200:
        raise SmokeFailure("settings API unavailable before admin PIN check")

    if not settings.get("admin", {}).get("pin_configured"):
        setup_url = urllib.parse.urljoin(base_url, "/api/admin/setup")
        print(f"Creating admin PIN through {setup_url}...", flush=True)
        status, data = http_json("POST", setup_url, timeout, payload={"pin": admin_pin})
        if status != 200 or not data.get("ok"):
            raise SmokeFailure(f"admin PIN setup failed with status {status}: {data}")
    else:
        check_url = urllib.parse.urljoin(base_url, "/api/admin/check")
        print(f"Checking admin PIN through {check_url}...", flush=True)
        status, data = http_json("POST", check_url, timeout, payload={}, headers=headers)
        if status != 200 or not data.get("ok"):
            raise SmokeFailure(f"admin PIN check failed with status {status}: {data}")

    status, data = http_json("POST", clear_url, timeout, payload={}, headers=headers)
    if status != 200 or not data.get("ok"):
        raise SmokeFailure(f"admin-protected events clear failed with status {status}: {data}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Flash and boot smoke test for gdo-blaq-homekit hardware.")
    parser.add_argument("--port", help="Serial port, for example /dev/cu.usbmodemXXXX or /dev/ttyACM0.")
    parser.add_argument("--auto-port", action="store_true", help="Auto-select the port only when exactly one likely ESP device is present.")
    parser.add_argument("--list-ports", action="store_true", help="List visible serial ports and exit.")
    parser.add_argument("--build", action="store_true", help="Run idf.py build before flashing or monitoring.")
    parser.add_argument("--flash", action="store_true", help="Run idf.py flash before monitoring.")
    parser.add_argument("--erase-flash", action="store_true", help="Run idf.py erase-flash before flash. This clears NVS/HomeKit data.")
    parser.add_argument("--reset", action="store_true", help="Toggle serial reset before monitoring. This is automatic after --flash.")
    parser.add_argument("--flash-baud", type=int, default=460800)
    parser.add_argument("--monitor-baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=90.0, help="Seconds to wait for boot smoke markers.")
    parser.add_argument(
        "--post-ready-seconds",
        type=float,
        default=0.0,
        help="Keep monitoring after boot markers are reached and fail if fatal logs appear.",
    )
    parser.add_argument(
        "--expect-wifi",
        choices=("any", "ap", "sta"),
        default="any",
        help="Expected Wi-Fi mode. Use ap after --erase-flash for provisioning smoke.",
    )
    parser.add_argument(
        "--probe-url",
        help="Optional HTTP URL to probe after serial smoke passes, for example http://192.168.4.1:8080/.",
    )
    parser.add_argument(
        "--admin-pin",
        default=os.environ.get("GDO_ADMIN_PIN"),
        help="Optional admin PIN for protected API probes. Defaults to GDO_ADMIN_PIN.",
    )
    parser.add_argument("--probe-timeout", type=float, default=5.0)
    parser.add_argument("--no-print-log", dest="print_log", action="store_false", help="Do not stream serial logs to stdout.")
    parser.set_defaults(print_log=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.erase_flash and not args.flash:
            raise SmokeFailure("--erase-flash requires --flash")
        if args.list_ports:
            for line in known_serial_ports():
                print(line)
            return 0

        port = resolve_port(args)
        maybe_build(args)
        maybe_flash(args, port)

        state = monitor_boot(args, port)
        if args.probe_url:
            probe_http(args.probe_url, args.probe_timeout)
            probe_status_api(args.probe_url, args.probe_timeout, args.admin_pin)
            probe_management_apis(args.probe_url, args.probe_timeout, args.admin_pin)

        wifi_mode = "ap" if state.ap_ready else "sta" if state.sta_ready else "unknown"
        print(f"PASS: boot smoke markers reached; wifi={wifi_mode}; http_server={state.http_ready}")
        return 0
    except subprocess.CalledProcessError as exc:
        print(f"FAIL: command exited with {exc.returncode}: {' '.join(exc.cmd)}", file=sys.stderr)
    except SmokeFailure as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
    except KeyboardInterrupt:
        print("Interrupted", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
