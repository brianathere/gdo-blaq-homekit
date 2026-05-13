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

## Port 8080 OTA Update

The first OTA-capable build must be flashed over USB because it changes the partition table from a single app slot to `ota_0`/`ota_1`. After that, app-only firmware updates can be installed over port 8080:

```sh
GDO_ADMIN_PIN=<web-admin-password> python tests/ota_update.py \
  --base-url http://<device-ip>:8080 \
  --bin build/gdo-blaq-homekit.bin
```

You can also open `http://<device-ip>:8080/ota` in a browser. The OTA endpoint requires the 8080 admin password to be configured first.

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
- `tests/ota_update.py` verifies the running OTA slot, uploads the firmware image to `/api/ota`, and waits for the device to reboot into the alternate slot.

## Host Protocol Checks

These checks exercise the pure `gdolib/secplus.c` protocol helpers without ESP-IDF hardware dependencies.

## Host App Harness

The `tests/host` harness builds selected app and dependency code as a native macOS/Linux binary with minimal ESP-IDF mocks. It covers the event ring buffer, settings persistence/apply flow, HomeKit TLV helpers, and `gdolib/secplus.c` under ASan/UBSan.

Install Ninja on macOS:

```sh
brew install ninja
```

Run the deterministic host tests:

```sh
tests/host/run.sh test
```

Run short libFuzzer smoke tests:

```sh
tests/host/run.sh fuzz
```

Run Cppcheck using the generated compile database:

```sh
tests/host/run.sh analyze
```

Build and run in Docker when local Clang/CMake versions are a problem:

```sh
docker build -f tests/host/Dockerfile -t gdo-blaq-host-tests .
docker run --rm gdo-blaq-host-tests
```

CBMC bounded smoke check:

```sh
cbmc tests/formal/secplus_cbmc_harness.c components/gdolib/secplus.c \
  -I components/gdolib \
  --function secplus_smoke_harness \
  --unwind 40 \
  --unwinding-assertions \
  --bounds-check \
  --pointer-check \
  --conversion-check \
  --signed-overflow-check \
  --unsigned-overflow-check
```

libFuzzer with ASan/UBSan:

```sh
/opt/homebrew/opt/llvm/bin/clang -std=c11 -g -O1 \
  -fsanitize=fuzzer,address,undefined \
  -Icomponents/gdolib \
  components/gdolib/secplus.c tests/fuzz/secplus_fuzz.c \
  -o .cache/analysis/secplus_fuzz

.cache/analysis/secplus_fuzz -runs=10000
```
