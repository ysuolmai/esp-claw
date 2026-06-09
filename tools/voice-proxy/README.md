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

The ESP32 must connect to the host machine's LAN IP, not `localhost`. For
example, if the host running Docker is `192.168.1.10`, the ESP32 WebSocket URL
is:

```text
ws://192.168.1.10:8080/ws/voice
```

## Configure sherpa-onnx Models

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

Echo mode is the default and does not call an LLM:

```yaml
REPLY_MODE: echo
```

OpenAI-compatible mode calls a chat-completions API after STT:

```yaml
REPLY_MODE: openai-compatible
OPENAI_BASE_URL: https://api.example.com/v1
OPENAI_MODEL: qwen2.5:7b
OPENAI_API_KEY: sk-...
```

`OPENAI_BASE_URL` can be any OpenAI-compatible provider base URL. It does not
need to be the official OpenAI endpoint. The proxy posts to
`$OPENAI_BASE_URL/chat/completions`.

If your provider exposes a non-standard full endpoint, set this instead:

```yaml
OPENAI_CHAT_COMPLETIONS_URL: https://api.example.com/custom/chat
```

Backward-compatible aliases are also accepted:

```yaml
LLM_BASE_URL: https://api.example.com/v1
LLM_MODEL: qwen2.5:7b
LLM_API_KEY: sk-...
LLM_CHAT_COMPLETIONS_URL: https://api.example.com/custom/chat
```

## ESP32 Lua Test

Set `voice_server_url` in Web Admin to the LAN URL, then press and hold BOOT on
the ESP32-S3 SuperMini to talk. Release BOOT to send `listen/stop`; the proxy
runs STT/reply/TTS and streams reply Opus back to the speaker.

You can also run the built-in script from the ESP-Claw console or Web Lua
runner:

```lua
lua --run --path /system/scripts/voice_stream_supermini.lua --args '{"uri":"ws://192.168.1.10:8080/ws/voice","record_ms":5000,"playback_timeout_ms":15000}'
```

Use the IP address of the machine running this proxy.
