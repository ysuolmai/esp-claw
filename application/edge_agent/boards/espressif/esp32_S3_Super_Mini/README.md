# ESP32-S3 SuperMini Quick Start

This board profile targets the ESP32-S3 SuperMini / ESP32-S3FH4R2 variant with
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

## Firmware Artifacts

The GitHub Actions workflow `Build ESP32-S3 SuperMini` publishes two artifacts:

- `esp32-s3-supermini-firmware`: normal build output, including individual
  binaries and `merged-binary.bin`.
- `esp32-s3-supermini-upgrade-no-settings`: upgrade package that updates the
  bootloader, partition table, app, and system image while preserving the old
  settings area and the writable storage partition.

For a normal first flash, use the individual binaries from
`esp32-s3-supermini-firmware`:

```bash
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x0 bootloader/bootloader.bin \
  0x9000 partition_table/partition-table.bin \
  0x20000 edge_agent.bin \
  0x300000 system.bin \
  0x3c0000 storage.bin
```

`merged-binary.bin` can also be flashed at `0x0`:

```bash
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x0 merged-binary.bin
```

The merged image ends at `0x3c0000`; it intentionally does not contain the
writable `storage` partition.

For an upgrade that keeps existing settings, download
`esp32-s3-supermini-upgrade-no-settings` and run the command included in
`upgrade-no-settings-command.txt`.

## Wi-Fi Provisioning

On first boot, or when no STA credentials are saved, the firmware starts a
provisioning AP:

```text
SSID: esp-claw-xxxxxx
IP:   http://192.168.4.1/
Auth: open by default, WPA2 if ap_password is configured
```

Connect a phone or computer to that AP, then open `http://192.168.4.1/`.
The captive portal DNS is enabled, so many clients will also show a setup page
automatically.

Set these fields in the Web UI:

- `wifi_ssid`
- `wifi_password`
- optional `ap_ssid`
- optional `ap_password`
- optional `ap_behavior`

`ap_behavior` accepts:

- `keep`: keep AP mode available after STA connects.
- `close_on_sta`: close AP mode after STA gets an IP.

Saving Web UI config stores the values in NVS. Restart the board to apply Wi-Fi,
core LLM, capability, and Lua module changes.

You can also configure Wi-Fi from the serial console:

```text
wifi --status
wifi --scan
wifi --set --ssid MyWiFi --password MyPassword --apply
```

## Device LLM Base URL

ESP-Claw's on-device Agent uses its own LLM settings. For a non-official
OpenAI-compatible API, set:

```text
llm_backend_type = openai_compatible
llm_base_url     = https://api.example.com/v1
llm_model        = your-model-name
llm_api_key      = your-key
llm_auth_type    = bearer
```

These fields can be saved from the Web UI or by posting to `/api/config`:

```bash
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -d '{
    "llm_backend_type": "openai_compatible",
    "llm_base_url": "https://api.example.com/v1",
    "llm_model": "your-model-name",
    "llm_api_key": "your-key",
    "llm_auth_type": "bearer"
  }'
```

This is separate from the voice proxy settings below.

## Voice Server

The firmware includes `/system/scripts/voice_stream_supermini.lua`. It streams
16 kHz mono Opus frames to a LAN WebSocket server and plays Opus frames returned
by that server.

Start the reference voice proxy on a LAN machine:

```bash
cd tools/voice-proxy
docker compose up --build
```

Health check:

```bash
curl http://localhost:8080/health
```

Configure sherpa-onnx model paths in `tools/voice-proxy/docker-compose.yml`:

```yaml
SHERPA_ASR_PARAFORMER: /models/sherpa-onnx-paraformer-zh-2023-09-14/model.int8.onnx
SHERPA_ASR_TOKENS: /models/sherpa-onnx-paraformer-zh-2023-09-14/tokens.txt
SHERPA_TTS_MODEL: /models/vits-zh/model.onnx
SHERPA_TTS_TOKENS: /models/vits-zh/tokens.txt
SHERPA_TTS_DATA_DIR: /models/vits-zh/espeak-ng-data
SHERPA_TTS_LEXICON: /models/vits-zh/lexicon.txt
```

If the proxy should call a non-official OpenAI-compatible API after STT, use:

```yaml
REPLY_MODE: openai-compatible
OPENAI_BASE_URL: https://api.example.com/v1
OPENAI_MODEL: your-model-name
OPENAI_API_KEY: your-key
```

If your provider does not expose the standard `/chat/completions` path, set the
full endpoint instead:

```yaml
OPENAI_CHAT_COMPLETIONS_URL: https://api.example.com/custom/chat
```

Run a push-to-talk style test from the ESP-Claw console or Web Lua runner.
Replace `192.168.1.10` with the LAN IP of the machine running Docker:

```lua
lua --run --path /system/scripts/voice_stream_supermini.lua --args '{"uri":"ws://192.168.1.10:8080/ws/voice","record_ms":5000}'
```

The current script records for `record_ms`, sends `listen/stop`, waits for TTS,
and then exits. It is a bring-up test path; always-on VAD and wake-word handling
are not implemented in this board profile yet.
