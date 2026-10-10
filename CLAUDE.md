# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP-IDF components and board examples for running ESP32 boards as [OpenClaw Nodes](https://docs.openclaw.ai/nodes) over WebSocket. Target toolchain is ESP-IDF **5.5.5** (CI pins `v5.5.5`; Tab5 uses a pinned `release-v5.5` image digest). Clone with submodules: `third_party/esp-webrtc-solution` and `third_party/esp-protocols` are pinned submodules consumed via `override_path` in example `idf_component.yml` files.

## Layering

- `components/esp-openclaw-node/` — the core component, the only one published to the IDF Component Registry (`espressif/esp-openclaw-node`). Public API is `include/esp_openclaw_node.h` (`esp_openclaw_node_*` prefix); internals split across `src/` by concern (identity/NVS seed, persisted session, connect-source resolution, protocol/connect signing, registry of capabilities+commands, runtime task/work queue, transport). Key contracts:
  - One connect attempt at a time, each ending in exactly one terminal event (`CONNECT_FAILED` / `DISCONNECTED`). **No automatic reconnect** — retry policy belongs to the application.
  - Explicit connect sources (setup code, token, password, no-auth) preempt in-flight/active sessions; automatic reconnect loops must use `SAVED_SESSION` so they never displace operator-issued requests.
  - Registration is only allowed while idle (no session, no request in flight).
  - Command handlers run synchronously on the component task; `*out_payload_json` must be `malloc()`-compatible and is freed by the component.
  - Limits are Kconfig options under `Component config -> ESP OpenClaw Node`.
  - `esp_openclaw_node_transport_start_begin/_end` are weak diagnostic hooks; the Tab5 build overrides them and CI checks they are defined in the ELF.
- `components/esp-openclaw-talk/` — adapter from OpenClaw Gateway-owned Talk API to `esp_webrtc` signaling (requires a connected operator session with `operator.talk`/`operator.read`). Refuses HTTP redirects on the offer POST by design.
- `components/esp-openclaw-node-provisioning/` — shared Wi-Fi station/NVS creds, USB REPL, setup-code commands, saved-session reconnect, and canonical `device.*`/`wifi.status` handlers. Board must prepare network hardware first (works with native Wi-Fi or `esp_wifi_remote`).
- `components/esp-openclaw-room-node/` — the canonical dual node/operator-session "room node" product (Talk lifecycle, wake word, Canvas/A2UI, face UI, diagnostics overlay, file commands). Boards plug in through narrow ports (display/input, audio, board services, optional storage); codec/pin/panel details stay in the examples.
- `examples/` — thin board apps. They pull local components via `EXTRA_COMPONENT_DIRS` in the project `CMakeLists.txt` and/or `override_path` in `main/idf_component.yml`.

## Building

From an example (or the Unity test app) directory with ESP-IDF exported:

```sh
idf.py set-target esp32s3   # esp32p4 for m5stack-tab5-room-node
idf.py build
```

CI matrix (`.github/workflows/ci.yml`): `components/esp-openclaw-node/test_apps/esp_openclaw_node_unity_tests` (build-only, esp32s3), `examples/esp32-node`, `examples/esp-box-3-display`, `examples/waveshare-esp32-s3-touch-amoled-2.06-room-node`, `examples/lichuang-szp-esp32-s3-room-node` (esp32s3), `examples/m5stack-tab5-room-node` (esp32p4).

**Tab5 is special:** it builds against a patched SDK. Before `idf.py reconfigure`/build, apply `scripts/idf_tab5_compat.py --idf-path "$IDF_PATH"` (patches in `patches/esp-idf/`), then after reconfigure apply `scripts/tab5_sdio_diagnostics.py` and `scripts/tab5_camera_compat.py` against `managed_components` (patches in `patches/esp-hosted/`, `patches/esp-video/`). Each script supports `--verify-only`. See `examples/m5stack-tab5-room-node/README.md` and the CI Build step for exact flags. Pinned component versions in that example (e.g. `esp_video 2.4.1`, `esp_hosted 1.4.0`) are tied to these patches — don't bump them casually.

## Tests

Most tests are host-side (no hardware) and mirror CI; run from repo root unless noted.

- Python script tests: `python3 -m unittest discover -s scripts/tests -p 'test_package_firmware.py' -v` (swap the pattern for one file; add `-k <name>` for one test).
- Room-node host C tests compile directly, e.g.:
  ```sh
  cc -std=c11 -Wall -Wextra -Werror -I components/esp-openclaw-room-node \
    components/esp-openclaw-room-node/room_file_validation.c \
    components/esp-openclaw-room-node/tests/test_room_file_validation.c -o /tmp/t && /tmp/t
  ```
  Same pattern for `room_diagnostics_metrics` and `room_pcm_gain` (+`-lm`). `python3 components/esp-openclaw-room-node/tests/test_room_audio_port_compat.py` too.
- Runner scripts that need `IDF_PATH` and/or `managed_components` from a prior build (cJSON defaults to the Unity test app's `managed_components`, so build that app first):
  - `components/esp-openclaw-talk/tests/run_host_tests.py --sanitize` (`--filter <name>` for one case), `run_threaded_tests.py` (`--tsan`), `run_http_host_tests.py`
  - `components/esp-openclaw-room-node/tests/run_file_host_tests.py` (`--filter`)
  - `components/esp-openclaw-room-node/tests/run_lifecycle_host_tests.py --managed-components <example>/managed_components` (`--case <name>`, `--list`)
  - `components/esp-openclaw-room-node/tests/run_ui_host_tests.py --lvgl-dir <example>/managed_components/lvgl__lvgl`
  - In CI, the Talk/file runners run from the Unity test app dir; lifecycle/UI runners run from the Waveshare example dir.
- On-device Unity tests: `components/esp-openclaw-node/test_apps/esp_openclaw_node_unity_tests` (CI only builds it).
- Release checker: `uv run --no-project --with PyYAML==6.0.3 python -m unittest discover -s scripts/tests -p 'test_check_component_release.py'`.

## Firmware packaging

`scripts/package_firmware.py` turns a build into `build/firmware/` (all flash images, `FLASHING.md`, `manifest.json` provenance, `SHA256SUMS`). It refuses credential-bearing configs and secure-boot/encrypted builds. Tab5 packaging requires the matching `--nvs-diagnostics --sdio-diagnostics --sdio-psram-rx --camera-compat` flags.

## Releases and changelog

- Changes go under `## Unreleased` in `components/esp-openclaw-node/CHANGELOG.md`; entries credit contributors and reference PR numbers, and release sections state their qualification scope (what was and wasn't hardware-tested).
- Registry upload is a manual workflow dispatch (`upload_component.yml`) with `expected_version` matching `idf_component.yml`; version bumps are separate reviewed changes. Details in `docs/component-releases.md`.
