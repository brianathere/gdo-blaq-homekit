# Hardware Smoke Tests

These tests are host-side checks for a real ESP32-S3 GDO blaQ device. They are intentionally not part of normal CI because they flash hardware.

## List Ports

```sh
. /Users/brianmeek/scratch/esp-idf-v6.0.1/export.sh
python tests/hardware_smoke.py --list-ports
```

## Fresh Provisioning Smoke

This erases flash, flashes the current build, resets into a fresh serial capture, waits for the app to boot, verifies the default provisioning AP path, and optionally probes the HTTP page. The HTTP probe only works after the host computer is connected to the `konnected-blaq-hk` Wi-Fi network.

```sh
. /Users/brianmeek/scratch/esp-idf-v6.0.1/export.sh
python tests/hardware_smoke.py \
  --port /dev/cu.usbmodemXXXX \
  --build \
  --flash \
  --erase-flash \
  --expect-wifi ap
```

Optional provisioning page probe:

```sh
python tests/hardware_smoke.py \
  --port /dev/cu.usbmodemXXXX \
  --expect-wifi ap \
  --probe-url http://192.168.4.1:8080/
```

Optional protected API probe:

```sh
GDO_ADMIN_PIN=123456 python tests/hardware_smoke.py \
  --port /dev/cu.usbmodemXXXX \
  --expect-wifi ap \
  --probe-url http://192.168.4.1:8080/
```

## Provisioned STA Smoke

Use this after Wi-Fi credentials are already stored in NVS:

```sh
python tests/hardware_smoke.py \
  --port /dev/cu.usbmodemXXXX \
  --flash \
  --expect-wifi sta \
  --post-ready-seconds 35 \
  --probe-url http://<device-ip>:8080/
```

## What It Verifies

- Flash command exits successfully when `--flash` is used.
- Firmware boots without fatal panic/assert/backtrace markers.
- App startup reaches `test_main: GDO started!`.
- Health supervisor startup reaches `app_health: Reset reason:`.
- `--post-ready-seconds` keeps monitoring after readiness and fails on delayed fatal logs such as task watchdog backtraces.
- Wi-Fi reaches the expected mode:
  - AP/provisioning mode: `konnected-blaq-hk` starts and HTTP handlers register on port 8080.
  - STA mode: the device connects, receives an IP, and starts the HTTP handlers on port 8080.
- Optional HTTP probe confirms the dashboard responds and, when reachable, validates `/api/status` JSON contains `app`, `gdo`, `wifi`, and `heap` sections.
- Optional HTTP probe also validates `/api/events`, `/api/settings`, `/api/homekit/setup`, and confirms protected mutation APIs reject missing admin credentials.
- When `--admin-pin` or `GDO_ADMIN_PIN` is provided, the probe sets or checks the admin PIN and verifies an admin-protected event clear request succeeds.
