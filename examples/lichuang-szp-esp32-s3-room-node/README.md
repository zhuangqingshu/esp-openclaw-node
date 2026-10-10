# LCSC SZP ESP32-S3 room node

A thin board adapter that runs the canonical `esp-openclaw-room-node` product
on the LCSC (Lichuang) "SZP" ESP32-S3 development board (立创·实战派ESP32-S3).
Dual sessions, reconnect, Talk, wake, Canvas/A2UI, the face, diagnostics, and
commands are shared with the Waveshare and Tab5 room nodes; see
[the Waveshare example](../waveshare-esp32-s3-touch-amoled-2.06-room-node/README.md)
for the full runtime and command reference.

This example owns only board details:

- **I/O expander.** A PCA9557 (I2C `0x19`) drives the LCD chip select, the
  NS4150 speaker amplifier enable, and the camera power-down. The board
  services `prepare_runtime` hook latches safe levels (LCD deselected,
  amplifier off, camera powered down) before the display or audio start.
- **Display.** 2.0" 320x240 ST7789 on SPI3 (MOSI 40, SCLK 41, DC 39, no reset
  line), hardware-rotated to landscape, with an active-low PWM backlight on
  GPIO 42. The panel uses SPI mode 2 at 40 MHz, inside the ST7789 write-clock
  limit. Because chip select lives on the expander, one NOP is clocked while
  the panel is deselected so SCLK is parked at its idle-high level; selecting
  first would turn the first idle transition into a phantom clock edge and
  misalign every command. The panel is then selected, software-reset, and
  given the 120 ms the controller needs before `SLPOUT`. LVGL renders through
  `esp_lvgl_port` into two 8-row internal DMA chunks and runs its task on a
  PSRAM stack, leaving enough internal RAM for the operator session's task
  stacks. The face runs at 30 Hz
  (`LV_DEF_REFR_PERIOD=33`) because a full SPI frame costs roughly 31 ms of
  bus time.
- **Touch.** FT6336 (FT5x06 family) on the shared I2C bus. Tapping or the BOOT
  button toggles Canvas/status; long-press opens Diagnostics.
- **Audio.** ES8311 playback and ES7210 capture on one 24 kHz stereo STD I2S
  pair (MCLK 38, BCLK 14, WS 13, DOUT 45, DIN 12), I2C SDA 1 / SCL 2. Capture
  selects MIC1 plus the speaker-reference loopback on MIC3, fed to the AFE as
  `"MR"`. The amplifier is enabled once the audio path is open.

- **Camera.** A DVP sensor on the shared I2C bus (SCCB): board batches ship
  either a GC0308 (VGA) or a GC2145 (UXGA); the driver probes which one is
  fitted. The PCA9557 keeps it powered down except during `camera.snap`. The
  driver bounces DMA chunks through internal RAM, so a 12 KiB block is
  reserved at boot and lent to the driver per capture (streaming straight into
  PSRAM drops bytes while the wake-word AFE loads the PSRAM bus). Talk borrows
  the block for each call, and a mid-call capture allocates its own; AFE
  reconfiguration at call start/end waits for a running capture to finish so
  the two never compete for contiguous internal RAM.
- **IMU.** QMI8658 at 0x6A/0x6B, polled at 50 Hz for gestures (see below).

The microSD slot is not used yet; the node advertises no file surface.

## Desk assistant

This example enables the room node's optional desk surface
(`esp_openclaw_room_desk_port_t`, see the
[room-node README](../../components/esp-openclaw-room-node/README.md#desk-assistant-surface)):
an idle clock synced over SNTP (`CONFIG_LICHUANG_DESK_TIMEZONE`, default
`CST-8`; `ntp.aliyun.com`), dimmed from 22:00 to 07:00, with Chinese text from
the `78/xiaozhi-fonts` PuHuiTi 16 px font. It adds these board commands:

| Command | Params | Notes |
| --- | --- | --- |
| `camera.list` | `{}` | One `szp-front` device. |
| `camera.snap` | `{maxWidth?, quality?, delayMs?, facing?, deviceId?, format?}` | JPEG; `maxWidth` picks 160/320/640/800/1024/1600 (GC0308 tops out at 640). About 3 s at 640 and 4.5 s at 1024. Works mid-call (`CONFIG_OPENCLAW_ROOM_CAMERA_DURING_TALK`); `BUSY` only while another capture runs. |
| `imu.status` | `{}` | Gravity vector, orientation, chip temperature, gesture counters. |

Local gestures from the IMU:

- **Knock twice** (on the board or the desk next to it) starts Talk, or ends
  the active call.
- **Lay it face down** for 1.5 s: do-not-disturb (ends any call, mutes the wake
  word, shows "勿扰"). Turning it back over lifts a face-down mute; a mute set
  with `desk.dnd` stays until cleared the same way.

### Gateway permissions

ESP32 nodes have no default command allowlist, so a freshly paired board shows
`Commands: (none effective)` in `openclaw nodes describe`. List the commands
you want agents to reach under `gateway.nodes.commands.allow` (`camera.snap` is
a privacy-heavy command and must also be absent from `commands.deny`), restart
the Gateway, then approve the widened surface with `openclaw nodes pending` /
`openclaw nodes approve <requestId>`.

## Talk backend

This example selects the **local voice bridge** Talk backend
(`CONFIG_OPENCLAW_ROOM_TALK_BACKEND_BRIDGE`, see the
[room-node README](../../components/esp-openclaw-room-node/README.md#local-voice-bridge-backend)):
"Hi ESP" or `talk.start` streams 16 kHz PCM to a voice bridge on the LAN, which
runs speech recognition, the agent turn, and speech synthesis and streams the
reply back. Point the board at the bridge once over the USB console:

```text
openclaw> bridge url ws://<bridge-host>:<port>
```

To use Gateway Talk (WebRTC realtime provider) instead, select
`OPENCLAW_ROOM_TALK_BACKEND_WEBRTC` in menuconfig.

## Build and provision

```sh
idf.py set-target esp32s3
idf.py build flash monitor
```

Provisioning is identical to the other room nodes over the USB console:

```text
openclaw> wifi set <ssid> <passphrase>
openclaw> gateway setup-code <code from `openclaw qr --voice-node --setup-code-only`>
openclaw> status
```

## Qualification status

Checked on one board with ESP-IDF 5.5.3:

- the ST7789 panel shows the Setup screen and the Diagnostics overlay in the
  correct landscape orientation;
- FT6336 touch opens and closes Diagnostics;
- the Diagnostics speaker test is audible through the ES8311/NS4150 path;
- setup-code pairing over `ws://` brings up both the node and the operator
  session, and both reconnect from NVS after a reboot;
- a spoken "Hi ESP" triggers WakeNet, and the AFE closes for Talk and reopens
  with WakeNet afterwards (repeated wake works);
- bridge Talk end to end: wake, a spoken question, the spoken reply,
  follow-up turns in the same session, the bridge's idle end, and wake again
  (two sessions, nine turns); no task watchdog, no renderer errors, and internal
  heap back at its boot level (about 57 KiB free) after the calls;
- an unreachable bridge fails after the 5 s connect timeout with "Bridge
  offline" and wake recovers.

Desk assistant, checked on the same board (a GC2145 batch): the idle clock,
SNTP time, Chinese date/ticker/timer rendering (via `canvas.snapshot`),
`desk.*` commands through `openclaw nodes invoke`, a voice-agent turn that set
a timer, `camera.snap` at 320/640/800/1024 px, and `imu.status` lying flat.
The notice card (on LVGL's top layer, which `canvas.snapshot` does not
capture), knock and face-down gestures, and the GC0308 variant were not
checked on hardware.

Not yet validated: Gateway (WebRTC) Talk on this board, echo cancellation
quality (the bridge is half duplex, so barge-in is not supported), wake false
accepts, speaker distortion at `playback_volume = 90` with full-scale speech,
and whether the MIC3 reference channel saturates at the shared 30 dB input
gain (the gain applies to the reference channel too; check the Diagnostics
meters). Recorded utterances arrive quiet (around -40 dBFS RMS).
