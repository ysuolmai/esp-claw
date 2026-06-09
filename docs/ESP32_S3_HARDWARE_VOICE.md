# ESP32-S3 SuperMini Voice Hardware

The SuperMini voice path is a lightweight streaming bring-up path. The ESP32-S3
captures and plays audio, but STT, LLM reply generation, and TTS run on an
external LAN server.

## Wiring

Use separate simplex I2S wiring, matching the Xiaozhi-style SuperMini layout:

| Device | Signal | GPIO |
| --- | --- | --- |
| INMP441 | WS / LRCL | GPIO4 |
| INMP441 | SCK / BCLK | GPIO5 |
| INMP441 | SD | GPIO6 |
| MAX98357A | DIN | GPIO11 |
| MAX98357A | BCLK | GPIO12 |
| MAX98357A | LRC | GPIO13 |

Use 3.3 V logic and keep the I2S wiring short. The MAX98357A speaker amplifier
needs a separate power path suitable for the speaker current.

## Firmware Role

The firmware provides:

- Audio codec/Lua module support for I2S bring-up
- Opus encoder and decoder support
- `/system/scripts/voice_stream_supermini.lua`
- Web Admin field `voice_server_url`
- BOOT/GPIO0 push-to-talk when STA Wi-Fi is connected

The SuperMini runtime path is push-to-talk style:

```text
press BOOT
record PCM16 mono at 16 kHz
encode Opus frames
stream frames over WebSocket
release BOOT
send listen/stop
receive reply audio frames
decode/play through I2S speaker
```

Always-on wake word, echo cancellation, and full-duplex VAD are intentionally
not implemented in this 4 MB profile.

## WebSocket Shape

The script is Xiaozhi-inspired but intentionally simple:

```text
client -> server: JSON hello/start metadata
client -> server: binary Opus audio frames
client -> server: JSON listen/stop
server -> client: binary Opus or PCM16 reply audio frames
server -> client: JSON done/error metadata
```

The recommended capture format is:

```text
sample rate: 16000 Hz
channels:    mono
sample:      signed 16-bit PCM before Opus
frame:       20-60 ms
```

Streaming avoids storing long WAV files in flash. Record length is mainly
limited by server timeout, WebSocket stability, RAM buffering, and VAD policy.

## Reference Voice Proxy

The repository includes a development proxy under `tools/voice-proxy`.

```bash
cd tools/voice-proxy
docker compose up --build
curl http://localhost:8080/health
```

Mount sherpa-onnx models into the container and set the relevant environment
variables in `docker-compose.yml`, for example:

```yaml
SHERPA_ASR_PARAFORMER: /models/sherpa-onnx-paraformer-zh-2023-09-14/model.int8.onnx
SHERPA_ASR_TOKENS: /models/sherpa-onnx-paraformer-zh-2023-09-14/tokens.txt
SHERPA_TTS_MODEL: /models/vits-zh/model.onnx
SHERPA_TTS_TOKENS: /models/vits-zh/tokens.txt
SHERPA_TTS_DATA_DIR: /models/vits-zh/espeak-ng-data
SHERPA_TTS_LEXICON: /models/vits-zh/lexicon.txt
```

For a third-party OpenAI-compatible API:

```yaml
REPLY_MODE: openai-compatible
OPENAI_BASE_URL: https://api.example.com/v1
OPENAI_MODEL: your-model-name
OPENAI_API_KEY: your-key
```

If the provider does not expose `/chat/completions`, set:

```yaml
OPENAI_CHAT_COMPLETIONS_URL: https://api.example.com/custom/chat
```

## Board Test

Set `voice_server_url` in Web Admin:

```text
ws://192.168.1.10:8080/ws/voice
```

After STA Wi-Fi is connected, hold BOOT while speaking and release BOOT to
finish the utterance. The firmware sends Opus frames while the button is held,
then waits for reply audio from the server and plays it through the MAX98357A.

The same Lua script can be launched manually for bring-up:

```lua
lua --run --path /system/scripts/voice_stream_supermini.lua --args '{"uri":"ws://192.168.1.10:8080/ws/voice","record_ms":5000,"playback_timeout_ms":15000}'
```

Use short tests first. A practical push-to-talk range is 5-20 seconds per
utterance; 30-60 seconds can work when Wi-Fi and the server are stable.
