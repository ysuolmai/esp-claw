import asyncio
import json
import logging
import os
import wave
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import httpx
import numpy as np
import opuslib
from fastapi import FastAPI, WebSocket, WebSocketDisconnect

try:
    import sherpa_onnx
except ImportError:  # pragma: no cover - keeps health/echo mode usable while debugging images
    sherpa_onnx = None


LOG = logging.getLogger("voice-proxy")
logging.basicConfig(level=os.getenv("LOG_LEVEL", "INFO"))

SAMPLE_RATE = int(os.getenv("SAMPLE_RATE", "16000"))
CHANNELS = int(os.getenv("CHANNELS", "1"))
FRAME_MS = int(os.getenv("FRAME_MS", "60"))
OPUS_BITRATE = int(os.getenv("OPUS_BITRATE", "24000"))
DATA_DIR = Path(os.getenv("DATA_DIR", "/data"))
DATA_DIR.mkdir(parents=True, exist_ok=True)


app = FastAPI(title="ESP-Claw Voice Proxy")


@dataclass
class SherpaRuntime:
    recognizer: Optional[object] = None
    tts: Optional[object] = None
    loaded: bool = False
    errors: list[str] = field(default_factory=list)


runtime = SherpaRuntime()


def _env(name: str) -> str:
    return os.getenv(name, "").strip()


def _load_paraformer_asr(num_threads: int) -> Optional[object]:
    if sherpa_onnx is None:
        runtime.errors.append("sherpa-onnx is not installed")
        return None

    paraformer = _env("SHERPA_ASR_PARAFORMER")
    tokens = _env("SHERPA_ASR_TOKENS")
    if not paraformer or not tokens:
        runtime.errors.append("ASR not configured: set SHERPA_ASR_PARAFORMER and SHERPA_ASR_TOKENS")
        return None

    if hasattr(sherpa_onnx.OfflineRecognizer, "from_paraformer"):
        return sherpa_onnx.OfflineRecognizer.from_paraformer(
            paraformer=paraformer,
            tokens=tokens,
            num_threads=num_threads,
            sample_rate=SAMPLE_RATE,
            feature_dim=80,
            debug=False,
        )

    feat_config = sherpa_onnx.FeatureConfig(sample_rate=SAMPLE_RATE, feature_dim=80)
    model_config = sherpa_onnx.OfflineModelConfig(
        paraformer=sherpa_onnx.OfflineParaformerModelConfig(model=paraformer),
        tokens=tokens,
        num_threads=num_threads,
        debug=False,
    )
    config = sherpa_onnx.OfflineRecognizerConfig(
        feat_config=feat_config,
        model_config=model_config,
    )
    return sherpa_onnx.OfflineRecognizer(config)


def _load_vits_tts(num_threads: int) -> Optional[object]:
    if sherpa_onnx is None:
        return None

    tts_model = _env("SHERPA_TTS_MODEL")
    tts_tokens = _env("SHERPA_TTS_TOKENS")
    if not tts_model or not tts_tokens:
        runtime.errors.append("TTS not configured: set SHERPA_TTS_MODEL and SHERPA_TTS_TOKENS")
        return None

    vits = sherpa_onnx.OfflineTtsVitsModelConfig(
        model=tts_model,
        lexicon=_env("SHERPA_TTS_LEXICON"),
        tokens=tts_tokens,
        data_dir=_env("SHERPA_TTS_DATA_DIR"),
    )
    model_config = sherpa_onnx.OfflineTtsModelConfig(
        vits=vits,
        num_threads=num_threads,
        debug=False,
    )
    config = sherpa_onnx.OfflineTtsConfig(
        model=model_config,
        max_num_sentences=1,
    )
    return sherpa_onnx.OfflineTts(config)


def load_sherpa() -> None:
    if runtime.loaded:
        return
    runtime.loaded = True
    num_threads = int(os.getenv("SHERPA_NUM_THREADS", "2"))

    try:
        runtime.recognizer = _load_paraformer_asr(num_threads)
        if runtime.recognizer is not None:
            LOG.info("Loaded sherpa-onnx Paraformer ASR")
    except Exception as exc:
        runtime.errors.append(f"ASR load failed: {exc}")
        LOG.exception("ASR load failed")

    try:
        runtime.tts = _load_vits_tts(num_threads)
        if runtime.tts is not None:
            LOG.info("Loaded sherpa-onnx VITS TTS")
    except Exception as exc:
        runtime.errors.append(f"TTS load failed: {exc}")
        LOG.exception("TTS load failed")


@app.on_event("startup")
async def startup() -> None:
    await asyncio.to_thread(load_sherpa)


@app.get("/health")
async def health() -> dict:
    return {
        "ok": True,
        "sample_rate": SAMPLE_RATE,
        "channels": CHANNELS,
        "frame_ms": FRAME_MS,
        "asr": runtime.recognizer is not None,
        "tts": runtime.tts is not None,
        "errors": runtime.errors,
    }


def pcm16_to_text(pcm: bytes) -> str:
    if runtime.recognizer is None:
        return ""
    samples = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
    stream = runtime.recognizer.create_stream()
    stream.accept_waveform(SAMPLE_RATE, samples)
    runtime.recognizer.decode_stream(stream)
    return stream.result.text.strip()


async def make_reply(text: str) -> str:
    mode = os.getenv("REPLY_MODE", "echo").strip().lower()
    if not text:
        return "I did not hear that clearly."
    if mode == "echo":
        return f"I heard: {text}"

    if mode == "openai-compatible":
        base_url = _env("LLM_BASE_URL").rstrip("/")
        api_key = _env("LLM_API_KEY")
        model = _env("LLM_MODEL") or "qwen2.5:7b"
        if not base_url:
            return f"Recognized: {text}"
        headers = {"Authorization": f"Bearer {api_key}"} if api_key else {}
        payload = {
            "model": model,
            "messages": [
                {"role": "system", "content": "You are a concise voice assistant. Keep replies short and suitable for speech."},
                {"role": "user", "content": text},
            ],
            "temperature": 0.7,
        }
        async with httpx.AsyncClient(timeout=60) as client:
            resp = await client.post(f"{base_url}/chat/completions", headers=headers, json=payload)
            resp.raise_for_status()
            data = resp.json()
            return data["choices"][0]["message"]["content"].strip()

    return f"Recognized: {text}"


def text_to_pcm16(text: str) -> bytes:
    if runtime.tts is None:
        return b""
    audio = runtime.tts.generate(
        text,
        sid=int(os.getenv("SHERPA_TTS_SID", "0")),
        speed=float(os.getenv("SHERPA_TTS_SPEED", "1.0")),
    )
    samples = np.asarray(audio.samples, dtype=np.float32)
    if audio.sample_rate != SAMPLE_RATE:
        raise RuntimeError(f"TTS sample rate {audio.sample_rate} != expected {SAMPLE_RATE}")
    pcm = np.clip(samples, -1.0, 1.0)
    return (pcm * 32767.0).astype(np.int16).tobytes()


def save_wav(path: Path, pcm: bytes) -> None:
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(pcm)


async def send_opus_pcm(websocket: WebSocket, pcm: bytes) -> int:
    encoder = opuslib.Encoder(SAMPLE_RATE, CHANNELS, opuslib.APPLICATION_VOIP)
    encoder.bitrate = OPUS_BITRATE
    frame_samples = SAMPLE_RATE * FRAME_MS // 1000
    frame_bytes = frame_samples * CHANNELS * 2
    sent = 0

    for offset in range(0, len(pcm), frame_bytes):
        chunk = pcm[offset : offset + frame_bytes]
        if len(chunk) < frame_bytes:
            chunk += b"\x00" * (frame_bytes - len(chunk))
        packet = encoder.encode(chunk, frame_samples)
        await websocket.send_bytes(packet)
        sent += 1
    return sent


@app.websocket("/ws/voice")
async def ws_voice(websocket: WebSocket) -> None:
    await websocket.accept()

    decoder = opuslib.Decoder(SAMPLE_RATE, CHANNELS)
    pcm_chunks: list[bytes] = []
    frame_samples = SAMPLE_RATE * FRAME_MS // 1000
    session_id = "esp-claw"
    listening = False

    try:
        while True:
            message = await websocket.receive()
            if message.get("type") == "websocket.disconnect":
                LOG.info("client disconnected")
                return

            if "text" in message and message["text"] is not None:
                text = message["text"]
                LOG.info("control: %s", text)
                try:
                    control = json.loads(text)
                    session_id = control.get("session_id") or session_id
                    if control.get("type") == "listen" and control.get("state") == "start":
                        pcm_chunks.clear()
                        listening = True
                    elif control.get("type") == "listen" and control.get("state") == "stop":
                        listening = False
                        break
                except json.JSONDecodeError:
                    pass
                continue

            data = message.get("bytes")
            if data and listening:
                pcm_chunks.append(decoder.decode(data, frame_samples))
    except WebSocketDisconnect:
        LOG.info("client disconnected before stop")
        return

    pcm = b"".join(pcm_chunks)
    if not pcm:
        await websocket.send_text(json.dumps({"type": "error", "message": "no audio"}))
        await websocket.send_text(json.dumps({"type": "done"}))
        return

    wav_path = DATA_DIR / f"{session_id}.wav"
    save_wav(wav_path, pcm)
    LOG.info("saved %s (%d bytes pcm)", wav_path, len(pcm))

    stt_text = await asyncio.to_thread(pcm16_to_text, pcm)
    reply = await make_reply(stt_text)
    await websocket.send_text(json.dumps({"type": "stt", "text": stt_text}, ensure_ascii=False))
    await websocket.send_text(json.dumps({"type": "reply", "text": reply}, ensure_ascii=False))

    try:
        tts_pcm = await asyncio.to_thread(text_to_pcm16, reply)
        if tts_pcm:
            frames = await send_opus_pcm(websocket, tts_pcm)
            LOG.info("sent %d opus TTS frames", frames)
        else:
            await websocket.send_text(json.dumps({"type": "error", "message": "TTS not configured"}))
    except Exception as exc:
        LOG.exception("TTS/send failed")
        await websocket.send_text(json.dumps({"type": "error", "message": str(exc)}))

    await websocket.send_text(json.dumps({"type": "done"}))
