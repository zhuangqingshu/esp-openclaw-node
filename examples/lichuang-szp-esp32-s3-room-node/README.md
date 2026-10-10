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

The camera (GC0308) and microSD slot are not used yet; the camera stays powered
down and the node advertises no file surface.

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

Not yet validated: Gateway (WebRTC) Talk on this board, echo cancellation
quality (the bridge is half duplex, so barge-in is not supported), wake false
accepts, speaker distortion at `playback_volume = 90` with full-scale speech,
and whether the MIC3 reference channel saturates at the shared 30 dB input
gain (the gain applies to the reference channel too; check the Diagnostics
meters). Recorded utterances arrive quiet (around -40 dBFS RMS).
