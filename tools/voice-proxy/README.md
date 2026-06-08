# ESP-Claw Voice Proxy

Local WebSocket voice bridge for ESP32-S3 SuperMini audio streaming.

It accepts ESP-Claw `audio.voice_stream()` traffic:

- JSON control frames: `hello`, `listen/start`, `listen/stop`
- Binary audio frames: raw Opus, 16 kHz, mono, usually 60 ms per frame

The proxy decodes incoming Opus to PCM, runs sherpa-onnx STT, creates a reply,
runs sherpa-onnx TTS, encodes the TTS PCM back to Opus, and streams it to the
ESP32.

## Start

```bash
cd tools/voice-proxy
docker compose up --build
```

Health check:

```bash
curl http://localhost:8080/health
```

## Configure Models

Place sherpa-onnx models under `tools/voice-proxy/models` and set env vars in
`docker-compose.yml`.

Example Paraformer ASR:

```yaml
SHERPA_ASR_PARAFORMER: /models/sherpa-onnx-paraformer-zh-2023-09-14/model.int8.onnx
SHERPA_ASR_TOKENS: /models/sherpa-onnx-paraformer-zh-2023-09-14/tokens.txt
```

Example VITS TTS:

```yaml
SHERPA_TTS_MODEL: /models/vits-zh/model.onnx
SHERPA_TTS_TOKENS: /models/vits-zh/tokens.txt
SHERPA_TTS_DATA_DIR: /models/vits-zh/espeak-ng-data
SHERPA_TTS_LEXICON: /models/vits-zh/lexicon.txt
```

The ESP32 stream currently expects TTS output at 16 kHz. Use a 16 kHz model, or
add resampling in `text_to_pcm16()`.

## Reply Modes

Default:

```yaml
REPLY_MODE: echo
```

The proxy speaks back a short echo of the recognized text.

OpenAI-compatible / Ollama-style chat:

```yaml
REPLY_MODE: openai-compatible
LLM_BASE_URL: http://host.docker.internal:11434/v1
LLM_MODEL: qwen2.5:7b
LLM_API_KEY:
```

## ESP32 Lua Test

Run the built-in script from the ESP-Claw console or Web Lua runner:

```lua
lua --run --path /system/scripts/voice_stream_supermini.lua --args '{"uri":"ws://192.168.1.10:8080/ws/voice","record_ms":5000}'
```

Use the IP address of the machine running this proxy.
