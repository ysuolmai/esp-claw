# ESP32-S3 SuperMini Quick Start

This board profile targets ESP32-S3 SuperMini / ESP32-S3FH4R2 boards with
4 MB flash and 2 MB PSRAM.

## Hardware

Audio wiring follows the `xiaozhi-esp32s3-supermini` style simplex I2S layout:

| Device | Signal | GPIO |
| --- | --- | --- |
| INMP441 | WS / LRCL | GPIO4 |
| INMP441 | SCK / BCLK | GPIO5 |
| INMP441 | SD | GPIO6 |
| MAX98357A | DIN | GPIO11 |
| MAX98357A | BCLK | GPIO12 |
| MAX98357A | LRC | GPIO13 |
| On-board WS2812 | RGB LED | GPIO48 |
| On-board BOOT | Button | GPIO0 |

The status LED stays off during normal operation and slowly blinks only while
the provisioning AP is active. The board power LED, if present, is usually
wired directly to power and is not firmware-controlled.

## Firmware Artifacts

The GitHub Actions workflow `Build ESP32-S3 SuperMini` publishes:

- `esp32-s3-supermini-firmware`: full build output, including the one-file
  first-flash image and individual binaries.
- `esp32-s3-supermini-upgrade-no-settings`: a normal upgrade package that does
  not touch NVS settings or writable storage.

### First flash on a blank board

For a blank board, use the one-file factory image:

```bash
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x0 esp-claw-esp32-s3-supermini-factory-full-merged.bin
```

This file includes bootloader, partition table, app, `system.bin`, and the
initial writable storage image. Because `system` and `storage` live near the end
of flash, this factory image is intentionally close to 4 MB.

### Upgrade without erasing settings

For normal upgrades after first flash, use the command included in
`upgrade-no-settings-command.txt`:

```bash
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x0 esp-claw-esp32-s3-supermini-bootloader.bin \
  0x9000 esp-claw-esp32-s3-supermini-partition-table.bin \
  0x20000 esp-claw-esp32-s3-supermini-app.bin \
  0x300000 esp-claw-esp32-s3-supermini-system.bin
```

There is deliberately no `0x0` merged upgrade image for this path. A `0x0`
merged image would contain padding across `0xA000-0x19FFF` and would erase the
64 KB NVS settings partition.

### Reset writable files

ESP-Claw uses a FAT writable storage partition at `0x3C0000`. The workflow also
uploads `spiffs.bin` as a compatibility alias for flashing tools that expect
that name, but it is the same FAT storage image.

Flash it only when you want to reset user files:

```bash
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x3c0000 esp-claw-esp32-s3-supermini-storage.bin
```

## Wi-Fi Provisioning

On first boot, or when no STA credentials are saved, the firmware starts a
provisioning AP:

```text
SSID: esp-claw-xxxxxx
IP:   http://192.168.4.1/
Auth: open by default, WPA2 if ap_password is configured
```

Connect a phone or computer to that AP, then open `http://192.168.4.1/`.
The captive portal DNS is enabled, so many clients will also show the setup page
automatically.

After STA Wi-Fi connects, the AP closes by default (`ap_behavior=close_on_sta`).
Hold BOOT for 5 seconds to restart into AP provisioning on the next boot. This
does not erase the saved SSID or password; it only skips STA for that boot.

## Web Admin Security

The Web Admin is open only while the board is in first-time/forced provisioning
AP mode. After STA networking is active, HTTP Basic Auth is required.

Credential priority:

1. NVS values saved from Web Admin.
2. Build-time defaults from Kconfig.
3. Generated fallback credentials.

The default username is `admin`. If no password is configured, the fallback
password is `esp-claw-XXXX`, where `XXXX` matches the AP suffix. The firmware
prints the active login hint to the serial log.

Secret fields such as Wi-Fi password, LLM API key, Telegram token, search keys,
and admin password are not returned by `/api/config`. Leaving a secret field
blank in Web Admin keeps the old value.

## OpenAI-Compatible Base URL

For a third-party OpenAI-compatible API, configure:

```text
llm_backend_type = openai_compatible
llm_base_url     = https://api.example.com/v1
llm_model        = your-model-name
llm_api_key      = your-key
llm_auth_type    = bearer
```

These fields are available in Web Admin and are stored in NVS.

## Telegram

Telegram is enabled in this SuperMini profile. Set `tg_bot_token` in Web Admin
or through `/api/config`. Empty token fields are ignored on save so an existing
token is not accidentally cleared by a partial update.

## Voice Server

The firmware includes `/system/scripts/voice_stream_supermini.lua`. It streams
16 kHz mono Opus frames to a LAN WebSocket server and plays Opus frames returned
by that server. STT/TTS runs on the external server, not on the ESP32-S3.

Start the reference proxy on a LAN machine:

```bash
cd tools/voice-proxy
docker compose up --build
```

Configure `voice_server_url` in Web Admin, for example:

```text
ws://192.168.1.10:8080/ws/voice
```

You can also run a push-to-talk bring-up test from the ESP-Claw console:

```lua
lua --run --path /system/scripts/voice_stream_supermini.lua --args '{"uri":"ws://192.168.1.10:8080/ws/voice","record_ms":5000}'
```

See `docs/ESP32_S3_SUPERMINI.md`, `docs/WIFI_ONBOARDING_AP.md`, and
`docs/ESP32_S3_HARDWARE_VOICE.md` for more details.
