# ESP-IDF 6.0.1 Upgrade Notes

This documents what changed to get this firmware building, flashing, and booting on ESP-IDF 6.0.1, what was patched in submodules, and which patches should become upstream pull requests instead of long-lived local submodule commits.

## Current State

- Superproject branch: `update/latest-submodules-idf6`.
- ESP-IDF: `v6.0.1`.
- Target: `esp32s3`.
- Local ESP-IDF checkout used for validation: `/Users/brianmeek/scratch/esp-idf-v6.0.1`.
- Main firmware build: green.
- Flash smoke test: passed on `/dev/cu.usbmodem1101` without erase flash.
- Submodule worktrees: clean local branches with one compatibility commit each.

Current submodule pins:

| Submodule | Upstream base | Local compatibility commit |
| --- | --- | --- |
| `components/gdolib` | `v1.0.0` / `bb84f61fd3b232b8a398d6432401ef3c00511cb7` | `3bdec25f008d4b09a365eedc8f9bb71cfeb66f18` |
| `components/nvs_wifi_connect` | upstream `master` / `8f9e1728b2464f5b1cfaf822a2f9fdaddbc25f89` | `087fded690b9666f6a31eb1d6a08732e5640d754` |
| `esp-homekit-sdk` | upstream `master` / `676fabac4a4a05184be020611cb069faa0016411` | `629ad08930ac8d2247e3b797b396662c7f8ce04b` |

Before this branch can be shared or merged reproducibly, the local submodule commits need to be pushed to fetchable remotes, or replaced by upstreamed commits.

## Superproject Changes

The superproject changes are product-specific and should stay in this repository:

- `.github/workflows/build.yml`
  - Updated CI from ESP-IDF `v5.4.1` to `v6.0.1`.
  - Updated combined-image generation from `esptool.py merge_bin` and underscore options to the ESP-IDF 6 era `esptool merge-bin` syntax with dash options.

- `main/idf_component.yml`
  - Raised the required IDF version from `>=5.4.1` to `>=6.0.1`.

- `sdkconfig.defaults`
  - Regenerated/comment-updated as ESP-IDF 6.0.1 project defaults.

- `main/CMakeLists.txt`
  - Added explicit `esp_event` and `esp_netif` dependencies.
  - Removed `wifi_provisioning` because this firmware does not use ESP-IDF's provisioning manager. It uses `nvs_wifi_connect` for Wi-Fi provisioning.

- `README.md`, `tests/README.md`, `tests/hardware_smoke.py`
  - Added hardware smoke test documentation and a host-side script that can build, flash, monitor boot logs, detect fatal boot failures, and verify the expected AP/STA provisioning path.

- `AGENTS.md`
  - Added repository guardrails, build instructions, ESP-IDF 6 migration notes, HomeKit/GDO safety notes, and smoke test workflow notes for future agents.

## Why `wifi_provisioning` Was Removed

This app never used the ESP-IDF `wifi_provisioning` manager APIs. Runtime provisioning is handled by `nvs_wifi_connect`:

- `main/wifi.cpp` calls `nvs_wifi_connect()` to connect from NVS or fall back to SoftAP.
- It calls `nvs_wifi_connect_start_http_server(NVS_WIFI_CONNECT_MODE_RESTART_ESP32, nullptr)` to serve the credential form.
- The user-facing flow is the AP `konnected-blaq-hk` and browser page at `http://192.168.4.1`.

In ESP-IDF 6, Espressif removed `wifi_provisioning` from ESP-IDF and renamed/moved that functionality to the external component `espressif/network_provisioning`. If this product later chooses to use Espressif's provisioning manager, the replacement path is:

```yaml
dependencies:
  espressif/network_provisioning:
    version: "^1.1.0"
```

That would be a provisioning behavior change. It is not required for the current firmware because `nvs_wifi_connect` is the provisioning implementation.

## Submodule Patches

### `components/gdolib`

Local commit: `3bdec25f008d4b09a365eedc8f9bb71cfeb66f18`

Patch:

- `CMakeLists.txt`
  - Replaced aggregate `driver` requirement with explicit `esp_driver_gpio` and `esp_driver_uart`.
  - Kept `esp_timer`.

Why it was needed:

- `gdolib` includes `driver/gpio.h` and `driver/uart.h`.
- ESP-IDF 6 no longer allows relying on older aggregate/transitive driver behavior for these headers. The component has to declare the split driver components directly.

Upstream PR recommendation:

- Open a small PR to `konnected-io/gdolib`.
- Do not submit the local patch exactly as-is if upstream still wants ESP-IDF v4.4 support. `gdolib` README says ESP-IDF v4.4+, and `esp_driver_gpio` / `esp_driver_uart` are not the right dependency names for older IDF 4.x projects. The upstream PR should use an IDF-version guard:
  - IDF 5+ / 6+: `esp_driver_gpio`, `esp_driver_uart`
  - IDF 4.4: `driver`

Additional finding:

- The `gdolib` standalone example does not build as checked out because `components/gdolib/examples/CMakeLists.txt` never adds the parent component directory, so the example cannot find `gdo.h`. That is separate from the firmware build and should be included in an upstream example/CI cleanup PR.

### `components/nvs_wifi_connect`

Local commit: `087fded690b9666f6a31eb1d6a08732e5640d754`

Patch:

- `CMakeLists.txt`
  - Added private requirements for `esp_event` and `esp_netif`.
  - Kept existing `nvs_flash` and `esp_wifi`.
  - Preserved the embedded HTML file.

Why it was needed:

- The component source and private header include and call `esp_event` and `esp_netif` APIs directly.
- Older builds got away with transitive dependencies; ESP-IDF 6 requires these to be declared.

Upstream PR recommendation:

- Open a small PR to `h2zero/nvs_wifi_connect`.
- This should be acceptable as a direct compatibility fix because the component already uses these APIs directly.

Additional finding:

- The `example_nvs_wifi_connect` example is not IDF 6 clean:
  - It requires `mdns`, which now needs to be pulled as a managed component, for example with `espressif/mdns`.
  - It directly includes/uses `esp_event` and `esp_netif` in the example source without declaring them in the example component.
- A second upstream PR should fix the example CMake/manifests so the example builds under IDF 6.

### `esp-homekit-sdk`

Local commit: `629ad08930ac8d2247e3b797b396662c7f8ce04b`

Patch:

- `components/button/CMakeLists.txt`
  - Replaced `driver` with `esp_driver_gpio`.

- `components/homekit/esp_hap_core/src/esp_hap_database.c`
  - Changed `hap_get_next_aid(char *id)` to `hap_get_next_aid(void)`.

- `components/homekit/esp_hap_core/src/priv_includes/esp_hap_database.h`
  - Changed old-style prototype `int hap_get_next_aid();` to `int hap_get_next_aid(void);`.

- `components/homekit/esp_hap_core/src/esp_hap_wifi.c`
  - Replaced `ESP_IF_WIFI_STA` / `ESP_IF_WIFI_AP` with `WIFI_IF_STA` / `WIFI_IF_AP`.

- `components/homekit/esp_hap_platform/CMakeLists.txt`
  - Stopped building `esp_mfi_i2c.c` for IDF 6.
  - Stopped adding legacy `driver` as a private requirement for IDF 6.

- `components/homekit/esp_hap_platform/src/esp_mfi_aes.c`
  - Replaced low-level mbedTLS AES usage with PSA Crypto AES-CTR setup/update/finish.
  - Added null checks for source and destination buffers.

- `components/homekit/esp_hap_platform/src/esp_mfi_sha.c`
  - Replaced low-level `mbedtls/sha1.h`, `mbedtls/sha256.h`, and `mbedtls/sha512.h` context usage with PSA Crypto hash operations for SHA-1, SHA-256, and SHA-512.

Why it was needed:

- ESP-IDF 6 moves to mbedTLS 4 and a PSA-first crypto API. The old low-level mbedTLS crypto headers/functions used by the HomeKit platform wrappers are no longer viable.
- ESP-IDF 6 tightens build/component dependencies and removes or moves several older interfaces.
- `ESP_IF_WIFI_*` is no longer the right type for `esp_wifi_get_config()` / `esp_wifi_set_config()`.
- The old `hap_get_next_aid()` declaration/definition mismatch breaks with the newer toolchain's stricter C handling.
- `esp_mfi_i2c.c` depends on legacy I2C driver headers. IDF 6 marks legacy I2C as end-of-life and it is on the removal path; the local firmware does not need that MFi I2C path.

Upstream PR recommendation:

- Do not open one giant PR. Split this into focused PRs:
  - PR 1: low-risk build/API compatibility.
    - `button` component driver dependency.
    - `WIFI_IF_*` replacements.
    - `hap_get_next_aid(void)` prototype cleanup.
  - PR 2: provisioning/example compatibility.
    - Port `examples/common/app_wifi` from `wifi_provisioning` to `espressif/network_provisioning`.
    - Rename API usage from `wifi_prov_*` to `network_prov_*`.
    - Add/adjust example manifests for `network_provisioning`.
    - Keep any required protocomm security config explicit.
  - PR 3: MFi platform crypto.
    - PSA Crypto migration for SHA and AES wrappers.
    - Add test vectors for SHA-1/SHA-256/SHA-512 and AES-CTR.
    - Review AES-CTR streaming semantics before submitting. The local AES patch is enough to compile this firmware, but it should be hardened before becoming an upstream crypto PR.
  - PR 4 or issue: MFi I2C.
    - Decide whether to port `esp_mfi_i2c.c` to the new I2C driver APIs or conditionally exclude it when not needed.
    - The local branch excludes it under IDF 6 because this firmware does not use that path. Upstream may need a real port for MFi users.
  - PR 5: CI.
    - Add an IDF 6 job to `esp-homekit-sdk/.gitlab-ci.yml` only after examples are ported.
    - Update README references from IDF release/v5.x to supported IDF ranges.

Additional findings:

- `esp-homekit-sdk` upstream currently has no release tags and no IDF 6 PR open that covers this migration.
- Its GitLab CI currently builds IDF 5.1 through 5.5, not IDF 6.
- `examples/common/app_wifi` now builds on IDF 6 using the managed `espressif/network_provisioning` component instead of the removed built-in `wifi_provisioning` component.
- The HomeKit example hardcoded credential path now uses `WIFI_IF_STA` instead of `ESP_IF_WIFI_STA`.

## CI And Configuration Findings

- The superproject GitHub Actions workflow is updated to IDF 6.0.1 and modern `esptool merge-bin` syntax.
- `actions/checkout@v4` uses `submodules: recursive`, so CI will only work once the local submodule commits are available from remotes.
- `dependencies.lock`, `managed_components/`, `sdkconfig`, and `build/` are ignored. That is normal for local ESP-IDF workflows, but it means CI resolves managed component versions at build time.
- The current IDF 6 build resolved:
  - `espressif/jsmn` `1.1.0`
  - `espressif/json_generator` `1.1.2`
  - `espressif/json_parser` `1.0.3`
  - `espressif/libsodium` `1.0.22`
  - `espressif/mdns` `1.11.1`
- The HomeKit examples also resolve `espressif/network_provisioning` `1.2.4` through `examples/common/app_wifi/idf_component.yml`.
- For release reproducibility, consider either checking in a root `dependencies.lock` or pinning managed component versions tightly in manifests. Today the root build is green, but future component registry updates could change CI behavior.
- `sdkconfig.defaults` uses `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y`. Current app size is close enough to matter but still has room.
- `idf.py size` reported:
  - app binary size `0x102200`
  - app partition free `0x74e00` bytes, 31%
  - IRAM `16384 / 16384` bytes, 100%
- IRAM is the main size pressure. Avoid new IRAM-resident handlers or config changes that move code into IRAM without checking size.

## Validation Performed

Current build validation:

```sh
. /Users/brianmeek/scratch/esp-idf-v6.0.1/export.sh
idf.py build
idf.py -C components/gdolib/examples set-target esp32s3 build
idf.py -C components/nvs_wifi_connect/example_nvs_wifi_connect set-target esp32s3 build
idf.py -C esp-homekit-sdk/examples/lightbulb set-target esp32s3 build
idf.py -C esp-homekit-sdk/examples/bridge set-target esp32s3 build
idf.py -C esp-homekit-sdk/examples/data_tlv8 set-target esp32s3 build
idf.py -C esp-homekit-sdk/examples/fan set-target esp32s3 build
idf.py -C esp-homekit-sdk/examples/smart_outlet set-target esp32s3 build
idf.py -C esp-homekit-sdk/examples/emulator set-target esp32s3 build
python3 -m py_compile tests/hardware_smoke.py
```

Current results:

- Product firmware build completed under ESP-IDF 6.0.1.
- Product app binary size is `0x102200`; the smallest app partition has `0x74e00` bytes free, 31%.
- `gdolib` example build completed after adding example component discovery and explicit dependency wiring.
- `nvs_wifi_connect` example build completed after adding managed `espressif/mdns`, explicit `esp_event` / `esp_netif`, and WebSocket support defaults.
- HomeKit examples `lightbulb`, `bridge`, `data_tlv8`, `fan`, `smart_outlet`, and `emulator` all built under ESP-IDF 6.0.1 after porting shared `app_wifi` to `espressif/network_provisioning`.
- The `emulator` example needed header prototype fixes for the newer compiler's stricter handling of empty parameter lists.

Hardware smoke validation from the earlier firmware bring-up:

```sh
. /Users/brianmeek/scratch/esp-idf-v6.0.1/export.sh
python tests/hardware_smoke.py \
  --port /dev/cu.usbmodem1101 \
  --build \
  --flash \
  --expect-wifi any
```

Smoke result:

- Build completed.
- Flash completed and verified bootloader, partition table, and app.
- Device booted.
- HomeKit initialized.
- Firmware logged `test_main: GDO started!`.
- Device connected in STA mode using existing NVS credentials and got IP `192.168.68.59`.
- Smoke script passed with `wifi=sta`.

Submodule probes now pass for the examples listed above. These fixes should still be opened upstream where they are generic IDF 6 compatibility issues rather than product-specific patches.

## Patch Vs Upstream Summary

Keep in this repository:

- IDF version bump and CI combined-image syntax.
- Main component dependency changes.
- Removal of unused `wifi_provisioning` from this app.
- Hardware smoke test script and docs.
- `AGENTS.md` operating guidance.

Open upstream PRs:

- `gdolib`: version-guarded driver component dependencies plus example CMake cleanup.
- `nvs_wifi_connect`: explicit `esp_event` / `esp_netif` dependencies, plus example managed-component fixes.
- `esp-homekit-sdk`: split PRs for core build/API fixes, `network_provisioning` example port, PSA Crypto migration, MFi I2C decision, and IDF 6 CI.

Avoid long-lived local patches for:

- Generic CMake dependency fixes in submodules.
- Generic IDF 6 API migrations in `esp-homekit-sdk`.
- Example build fixes that are upstream quality issues.

Acceptable temporary local patches:

- MFi I2C exclusion for this firmware branch, until upstream chooses whether to port or conditionally exclude it.
- PSA Crypto MFi wrapper migration, as long as it is treated as a temporary compile compatibility patch until test vectors and streaming semantics are reviewed.

## References

- ESP-IDF v6.0.1 release: https://github.com/espressif/esp-idf/releases/tag/v6.0.1
- ESP-IDF v6.0.1 release notes database: https://release-notes.espressif.tools/release/6.0.1
- ESP-IDF 6 provisioning migration: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/migration-guides/release-6.x/6.0/provisioning.html
- ESP-IDF 6 peripherals migration: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-6.x/6.0/peripherals.html
- ESP-IDF 6 security / PSA Crypto migration: https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32c61/migration-guides/release-6.x/6.0/security.html
- `espressif/network_provisioning` component: https://components.espressif.com/components/espressif/network_provisioning
- `gdolib` latest release: https://github.com/konnected-io/gdolib/releases/tag/v1.0.0
- `nvs_wifi_connect` upstream: https://github.com/h2zero/nvs_wifi_connect
- `esp-homekit-sdk` upstream: https://github.com/espressif/esp-homekit-sdk
