# Code Flow and Reliability Review

## Runtime Flow

- `app_main()` configures the GDO UART pins, starts `gdolib`, starts the app health supervisor, and then creates the HomeKit task.
- `gdolib` owns the UART event loop. `gdo_start()` installs the UART driver, creates `gdo_main_task`, starts an initial sync task, and sends GDO callbacks from the main task when decoded state changes.
- `homekit_task_entry()` builds the HomeKit accessory, initializes Wi-Fi through `nvs_wifi_connect`, starts HAP, and then drains the GDO notification queue into HomeKit characteristic updates.
- HomeKit writes call `gdo_door_*`, `gdo_lock/unlock`, and `gdo_light_*`. Those functions enqueue wireline commands and now propagate command queue/encoding failures back through HAP status codes.
- The Wi-Fi config/status dashboard is served by `nvs_wifi_connect` on port 8080 in both AP fallback mode and provisioned STA mode. App handlers in `main/wifi.cpp` add `GET /api/status` and read-only `POST /api/gdo/refresh`.

## Issues Fixed

- HomeKit write callbacks no longer report success when the underlying GDO command failed.
- Lock current/target values now use the HomeKit `uint8` value path instead of the boolean path.
- `gdolib` frees queued command packets on queue-send failure and deletes one-shot timers when start fails.
- `gdolib` now exposes read-only status probing and last-valid-RX timestamp helpers for app health monitoring.
- `nvs_wifi_connect` no longer logs Wi-Fi passwords, bounds SSID/password copies, times out initial STA connect, and avoids duplicate HTTP server/event-handler startup.
- The app health task now feeds the ESP task watchdog on a 1 second cadence while keeping expensive health checks on a 30 second cadence. This fixes a boot-time task watchdog panic caused by a 30 second sleep against a 10 second TWDT timeout.
- GDO sync recovery now caps automatic rolling-code jumps to three attempts per boot and logs old/new values. A disconnected or miswired GDO bus should no longer advance rolling code indefinitely.

## Health Supervisor

- `main/app_health.cpp` logs reset reason at boot, subscribes its task to the ESP-IDF task watchdog, and performs checks every 30 seconds.
- Wi-Fi recovery is conservative: request reconnect at most once per minute and reboot only after 30 minutes without STA IP.
- GDO recovery is conservative: request resync/status probes at most every 5 minutes and reboot only after 30 minutes unsynced or without valid GDO RX.
- The health supervisor never sends door, light, lock, learn, or clear-paired-device commands.

## Dashboard and Status API

- `GET /` returns the embedded dashboard. It shows door, protocol/sync, light, lock, motion, obstruction, motor, button, battery, learn, movement timing, paired devices, Wi-Fi mode/IP/RSSI/MAC data, uptime, reset reason, HomeKit state, heap, and build fields.
- `GET /api/status` returns the same device state as JSON. Unknown GDO percentages and paired-device counts are represented as `null`.
- `POST /api/gdo/refresh` queues a read-only status request through `gdolib`; it does not actuate the door or change lock/light state.

## Follow-Up Hardening

- The port-8080 config page is unauthenticated. For production garage deployments, add either an auth gate or a runtime setting that disables it outside provisioning windows.
- `gdolib` still mutates global `g_status` from several paths without a single writer lock strategy. `gdo_get_status()` copies under a spinlock, but writers are not consistently under that lock; centralizing status mutation is the next meaningful concurrency hardening task.
- Add a small event-history ring buffer and expose it through the dashboard/API so the last N door, Wi-Fi, HomeKit, reset, and GDO sync events are visible without serial logs.
- Add optional OTA/update support, Wi-Fi scan-and-select, dashboard auth, and a metrics endpoint for garage monitoring.
- Consider upstreaming the `gdolib` packet/timer cleanup and read-only health helpers, and the `nvs_wifi_connect` port/event-handler hardening.
- If CI time allows, add a host-side static check for forbidden credential logging and build-size regression thresholds.
