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
  --probe-url http://192.168.4.1/
```

## Provisioned STA Smoke

Use this after Wi-Fi credentials are already stored in NVS:

```sh
python tests/hardware_smoke.py \
  --port /dev/cu.usbmodemXXXX \
  --flash \
  --expect-wifi sta
```

## What It Verifies

- Flash command exits successfully when `--flash` is used.
- Firmware boots without fatal panic/assert/backtrace markers.
- App startup reaches `test_main: GDO started!`.
- Wi-Fi reaches the expected mode:
  - AP/provisioning mode: `konnected-blaq-hk` starts and HTTP handlers register.
  - STA mode: the device connects and receives an IP.
- Optional HTTP probe confirms the provisioning page responds.
