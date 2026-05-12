# HomeKit Firmware for the GDO blaQ
 This is a HomeKit native firmware for Konnected's GDO blaQ, a smart garage door opener control accessory for Chamberlain/LiftMaster/Craftsman/Merlin garage openers with Security+ or Security+2.0.
  
 ## Submodules
 
 This repository uses git submodules. After cloning, be sure to initialize and update them:
 
 ```sh
 git submodule update --init --recursive
 ```

## Provision WiFi and Add Accessory to HomeKit

 This firmware uses the `nvs_wifi_connect` component to manage WiFi connections. The relevant code can be found in `main/wifi.cpp`.
 
 The `nvs_wifi_connect` component starts an HTTP server for configuration and status on port 8080. Connect to the access point created by the device with SSID `konnected-blaq-hk` and open a web browser to `http://192.168.4.1:8080/` and enter your WiFi credentials, then click `Save and Restart`. On an already-provisioned device, open `http://<device-ip>:8080/`.

 The status dashboard shows door/GDO state, HomeKit health, heap, WiFi, build, uptime, reset reason, MAC/IP data, paired-device counts, timing/RX fields, diagnostics, GDO settings, and HomeKit setup data. The same core data is available as JSON at `http://<device-ip>:8080/api/status`.

Read-only APIs:

- `GET /api/status`
- `GET /api/events?limit=50&category=&severity=`
- `GET /api/settings`
- `GET /api/homekit/setup`
- `GET /api/homekit/setup-qr.svg`
- `POST /api/gdo/refresh`

Mutating APIs require an admin PIN in the `X-Admin-PIN` header:

- `POST /api/admin/setup`
- `POST /api/admin/check`
- `POST /api/events/clear`
- `POST /api/settings`
- `POST /api/gdo/sync`
- `POST /api/gdo/position`

 After connecting to WiFi and restarting wait about 10 seconds then open the Home app on your iOS device to add the accessory. Go to "Add Accessory" and then "more options..." to find the accessory on the network. Click on the found accessory and enter the setup code `251-02-023` when prompted and follow the instructions to complete the setup.

## Hardware Smoke Test

The host-side smoke test can build, flash, monitor serial boot logs, and verify the default provisioning path:

```sh
. /Users/brianmeek/scratch/esp-idf-v6.0.1/export.sh
python tests/hardware_smoke.py \
  --port /dev/cu.usbmodemXXXX \
  --build \
  --flash \
  --expect-wifi sta \
  --post-ready-seconds 35 \
  --probe-url http://<device-ip>:8080/
```

`--erase-flash` clears NVS and HomeKit pairing data. Use it only for fresh provisioning checks with `--expect-wifi ap`; omit it when testing an already-provisioned device. `--post-ready-seconds` keeps the serial monitor attached after boot markers so delayed watchdog/panic logs fail the test.
