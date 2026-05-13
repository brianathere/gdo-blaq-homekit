#!/usr/bin/env python3
"""Upload a firmware image to the device's port-8080 OTA endpoint."""

from __future__ import annotations

import argparse
import http.client
import json
import os
import sys
import time
import urllib.parse
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BIN = REPO_ROOT / "build" / "gdo-blaq-homekit.bin"


class OtaFailure(RuntimeError):
    pass


def parse_base_url(value: str) -> urllib.parse.ParseResult:
    parsed = urllib.parse.urlparse(value)
    if parsed.scheme != "http" or not parsed.hostname:
        raise OtaFailure("base URL must look like http://<device-ip>:8080")
    return parsed


def auth_headers(pin: str | None) -> dict[str, str]:
    return {"X-Admin-PIN": pin} if pin else {}


def request_json(base: urllib.parse.ParseResult, path: str, pin: str | None, timeout: float) -> dict:
    conn = http.client.HTTPConnection(base.hostname, base.port or 80, timeout=timeout)
    try:
        conn.request("GET", path, headers=auth_headers(pin))
        response = conn.getresponse()
        body = response.read().decode("utf-8", errors="replace")
    except OSError as exc:
        raise OtaFailure(f"GET {path} failed: {exc}") from exc
    finally:
        conn.close()

    if response.status >= 400:
        raise OtaFailure(f"GET {path} failed with HTTP {response.status}: {body}")
    try:
        return json.loads(body)
    except json.JSONDecodeError as exc:
        raise OtaFailure(f"GET {path} returned invalid JSON: {exc}") from exc


def upload_firmware(
    base: urllib.parse.ParseResult,
    firmware: Path,
    pin: str,
    chunk_size: int,
    timeout: float,
) -> dict:
    size = firmware.stat().st_size
    conn = http.client.HTTPConnection(base.hostname, base.port or 80, timeout=timeout)
    try:
        conn.putrequest("POST", "/api/ota")
        conn.putheader("Content-Type", "application/octet-stream")
        conn.putheader("Content-Length", str(size))
        conn.putheader("X-Admin-PIN", pin)
        conn.endheaders()

        sent = 0
        with firmware.open("rb") as handle:
            while True:
                chunk = handle.read(chunk_size)
                if not chunk:
                    break
                conn.send(chunk)
                sent += len(chunk)
                print(f"\ruploaded {sent}/{size} bytes", end="", flush=True)
        print()

        response = conn.getresponse()
        body = response.read().decode("utf-8", errors="replace")
    except OSError as exc:
        raise OtaFailure(f"OTA upload failed: {exc}") from exc
    finally:
        conn.close()

    if response.status >= 400:
        raise OtaFailure(f"OTA upload failed with HTTP {response.status}: {body}")
    try:
        return json.loads(body)
    except json.JSONDecodeError as exc:
        raise OtaFailure(f"OTA upload returned invalid JSON: {exc}") from exc


def partition_label(status: dict) -> str | None:
    ota = status.get("ota", {})
    running = ota.get("running_partition") or {}
    label = running.get("label")
    return label if isinstance(label, str) else None


def wait_for_reboot(
    base: urllib.parse.ParseResult,
    pin: str,
    old_partition: str | None,
    old_uptime_ms: int | None,
    timeout: float,
) -> dict:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        time.sleep(2.0)
        try:
            status = request_json(base, "/api/status", pin, timeout=5.0)
        except Exception as exc:  # device is normally offline during reboot
            last_error = exc
            continue

        app = status.get("app", {})
        new_uptime_ms = app.get("uptime_ms")
        new_partition = partition_label(status)
        rebooted = isinstance(new_uptime_ms, int) and (
            old_uptime_ms is None or new_uptime_ms < old_uptime_ms
        )
        changed_slot = old_partition is not None and new_partition and new_partition != old_partition
        if rebooted or changed_slot:
            return status

    detail = f"last error: {last_error}" if last_error else "device stayed reachable but did not reboot"
    raise OtaFailure(f"device did not come back from OTA before timeout; {detail}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", required=True, help="Device base URL, for example http://192.168.68.59:8080")
    parser.add_argument("--bin", default=str(DEFAULT_BIN), help="Firmware .bin path")
    parser.add_argument("--admin-pin", default=os.environ.get("GDO_ADMIN_PIN"), help="Admin password or GDO_ADMIN_PIN")
    parser.add_argument("--chunk-size", type=int, default=16 * 1024)
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--status-only", action="store_true")
    parser.add_argument("--no-wait", action="store_true")
    args = parser.parse_args()

    try:
        base = parse_base_url(args.base_url)
        firmware = Path(args.bin)
        if not args.status_only and not firmware.exists():
            raise OtaFailure(f"firmware image not found: {firmware}")
        if not args.status_only and not args.admin_pin:
            raise OtaFailure("OTA upload requires --admin-pin or GDO_ADMIN_PIN")

        status = request_json(base, "/api/status", args.admin_pin, timeout=10.0)
        ota = status.get("ota", {})
        print(json.dumps(ota, indent=2, sort_keys=True))
        if args.status_only:
            return 0
        if not ota.get("supported"):
            raise OtaFailure("device is not running an OTA partition table")
        if not ota.get("admin_pin_configured"):
            raise OtaFailure("configure the 8080 admin password before OTA")

        old_partition = partition_label(status)
        old_uptime_ms = status.get("app", {}).get("uptime_ms")
        print(f"uploading {firmware} to {base.geturl()} from slot {old_partition or 'unknown'}")
        result = upload_firmware(base, firmware, args.admin_pin, args.chunk_size, timeout=args.timeout)
        print(json.dumps(result, indent=2, sort_keys=True))

        if not args.no_wait:
            rebooted = wait_for_reboot(base, args.admin_pin, old_partition, old_uptime_ms, args.timeout)
            print("device returned after OTA")
            print(json.dumps(rebooted.get("ota", {}), indent=2, sort_keys=True))
        return 0
    except OtaFailure as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
