# ESP32-S3 SuperMini Firmware Profile

This profile is tuned for ESP32-S3 SuperMini / ESP32-S3FH4R2 boards with
4 MB flash and 2 MB PSRAM.

## Flash Layout

The 4 MB partition table is:

```text
0x00000  bootloader
0x09000  partition table
0x0A000  nvs       64 KB
0x1A000  phy_init  4 KB
0x20000  factory   2944 KB
0x300000 system    768 KB, FAT, firmware-bundled resources
0x3C0000 storage   256 KB, FAT, writable user files
```

NVS is intentionally 64 KB so Wi-Fi credentials, API keys, admin credentials,
Telegram token, Base URL, and voice server URL have room to grow.

## Artifacts

Use `esp-claw-esp32-s3-supermini-factory-full-merged.bin` only for first flash
or a full reset. It includes bootloader, partition table, app, system, and the
initial storage image. It is close to 4 MB because it has to cover high flash
offsets.

Use `esp32-s3-supermini-upgrade-no-settings` for normal upgrades. It writes
only these offsets:

```text
0x00000  esp-claw-esp32-s3-supermini-bootloader.bin
0x09000  esp-claw-esp32-s3-supermini-partition-table.bin
0x20000  esp-claw-esp32-s3-supermini-app.bin
0x300000 esp-claw-esp32-s3-supermini-system.bin
```

This preserves:

```text
0x0A000-0x19FFF NVS settings
0x3C0000-0x3FFFFF writable storage
```

Do not create a `0x0` merged image for no-settings upgrades. A merged file from
`0x0` to `0x20000` contains padding and would erase the NVS partition.

## Enabled Features

The SuperMini profile keeps the features that fit the 4 MB / 2 MB target:

- Web Admin and captive provisioning AP
- Telegram IM capability
- MCP client/server
- Scheduler
- LLM inspect
- Web search
- Lua filesystem/tools
- Opus encoder/decoder for the voice streaming script
- INMP441/MAX98357A bring-up through the audio Lua module
- BOOT/GPIO0 push-to-talk streaming when STA Wi-Fi and `voice_server_url`
  are configured

Large optional stacks such as Bluetooth, camera, display, LVGL, and local
on-device STT/TTS are disabled.

## Voice Path

Set `voice_server_url` in Web Admin, for example:

```text
ws://192.168.1.10:8080/ws/voice
```

After STA Wi-Fi is connected, hold BOOT while speaking and release BOOT to end
the utterance. The ESP32-S3 streams Opus frames to the LAN voice server and
plays the returned Opus audio through MAX98357A. STT/TTS and any LLM call run on
the external server, not on the ESP32-S3 flash.

Runtime BOOT is consumed by voice when STA Wi-Fi is connected and
`voice_server_url` is set. For recovery, hold BOOT while resetting or powering
on; the boot-time 5-second AP provisioning check still runs before voice starts.

## Build Locally

```bash
cd application/edge_agent
. /opt/esp/idf/export.sh
pip install esp-bmgr-assist
idf.py bmgr -c ./boards -b esp32_S3_Super_Mini
idf.py build
```

GitHub Actions uses the same board-manager flow in
`.github/workflows/build-supermini.yml`.
