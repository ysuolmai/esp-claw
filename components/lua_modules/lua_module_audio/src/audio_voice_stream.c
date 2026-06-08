/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio_private.h"

#include "esp_crt_bundle.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"

#define VOICE_STREAM_WS_BUFFER_SIZE      4096
#define VOICE_STREAM_WS_TASK_STACK       (12 * 1024)
#define VOICE_STREAM_CONNECT_TIMEOUT_MS  10000
#define VOICE_STREAM_SEND_TIMEOUT_MS     1000
#define VOICE_STREAM_MAX_FRAGMENT_BYTES  8192

typedef struct {
    audio_device_t *input;
    audio_device_t *output;
    int input_ref;
    int output_ref;
    char *uri;
    char *token;
    char *session_id;
    uint32_t sample_rate;
    uint32_t frame_ms;
    uint32_t record_ms;
    uint32_t playback_timeout_ms;
    uint8_t channels;
    uint8_t bits;
    int bitrate;
    int complexity;
    bool enable_vbr;
    bool enable_dtx;
    bool closed;
    bool running;
    volatile bool connected;
    volatile bool server_done;
    esp_websocket_client_handle_t ws;
    void *encoder;
    void *decoder;
    uint8_t *pcm_buf;
    uint8_t *opus_buf;
    uint8_t *dec_pcm_buf;
    uint8_t *rx_frag_buf;
    uint32_t rx_frag_size;
    uint32_t rx_frag_received;
    int pcm_size;
    int opus_size;
    uint32_t dec_pcm_size;
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint32_t tx_errors;
    uint32_t rx_errors;
} audio_voice_stream_t;

static audio_voice_stream_t *lua_audio_check_voice_stream(lua_State *L, int idx)
{
    audio_voice_stream_t *stream = (audio_voice_stream_t *)luaL_checkudata(L, idx, AUDIO_VOICE_STREAM_META);
    if (!stream || stream->closed) {
        luaL_error(L, "audio voice_stream: invalid or closed stream");
    }
    return stream;
}

static bool audio_voice_mem_contains(const char *data, int len, const char *needle)
{
    int needle_len = (int)strlen(needle);
    if (!data || len <= 0 || needle_len <= 0 || needle_len > len) {
        return false;
    }
    for (int i = 0; i <= len - needle_len; i++) {
        if (memcmp(data + i, needle, (size_t)needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool audio_voice_text_is_done(const char *data, int len)
{
    return audio_voice_mem_contains(data, len, "\"state\":\"stop\"") ||
           audio_voice_mem_contains(data, len, "\"state\": \"stop\"") ||
           audio_voice_mem_contains(data, len, "\"type\":\"done\"") ||
           audio_voice_mem_contains(data, len, "\"type\": \"done\"");
}

static char *audio_voice_get_string_field(lua_State *L, int idx, const char *name, const char *def)
{
    char *ret = NULL;
    idx = lua_absindex(L, idx);
    lua_getfield(L, idx, name);
    if (lua_isnil(L, -1)) {
        if (def) {
            ret = audio_strdup(def);
        }
    } else {
        const char *s = luaL_checkstring(L, -1);
        ret = audio_strdup(s);
    }
    lua_pop(L, 1);
    return ret;
}

static bool audio_voice_get_bool_field(lua_State *L, int idx, const char *name, bool def)
{
    bool ret = def;
    idx = lua_absindex(L, idx);
    lua_getfield(L, idx, name);
    if (!lua_isnil(L, -1)) {
        ret = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    return ret;
}

static esp_opus_enc_frame_duration_t audio_voice_enc_duration(uint32_t frame_ms)
{
    switch (frame_ms) {
    case 20:
        return ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    case 40:
        return ESP_OPUS_ENC_FRAME_DURATION_40_MS;
    case 60:
        return ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    default:
        return ESP_OPUS_ENC_FRAME_DURATION_ARG;
    }
}

static esp_opus_dec_frame_duration_t audio_voice_dec_duration(uint32_t frame_ms)
{
    switch (frame_ms) {
    case 20:
        return ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    case 40:
        return ESP_OPUS_DEC_FRAME_DURATION_40_MS;
    case 60:
        return ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    default:
        return ESP_OPUS_DEC_FRAME_DURATION_INVALID;
    }
}

static void audio_voice_cleanup_runtime(audio_voice_stream_t *stream)
{
    if (!stream) {
        return;
    }
    if (stream->ws) {
        esp_websocket_client_stop(stream->ws);
        esp_websocket_client_destroy(stream->ws);
        stream->ws = NULL;
    }
    if (stream->encoder) {
        esp_opus_enc_close(stream->encoder);
        stream->encoder = NULL;
    }
    if (stream->decoder) {
        esp_opus_dec_close(stream->decoder);
        stream->decoder = NULL;
    }
    free(stream->pcm_buf);
    free(stream->opus_buf);
    free(stream->dec_pcm_buf);
    free(stream->rx_frag_buf);
    stream->pcm_buf = NULL;
    stream->opus_buf = NULL;
    stream->dec_pcm_buf = NULL;
    stream->rx_frag_buf = NULL;
    stream->rx_frag_size = 0;
    stream->rx_frag_received = 0;
    stream->connected = false;
}

static esp_err_t audio_voice_decode_and_play(audio_voice_stream_t *stream, const uint8_t *data, uint32_t len)
{
    esp_audio_dec_in_raw_t raw = {
        .buffer = (uint8_t *)data,
        .len = len,
    };
    esp_audio_dec_out_frame_t frame = {
        .buffer = stream->dec_pcm_buf,
        .len = stream->dec_pcm_size,
    };
    esp_audio_dec_info_t info = {0};
    esp_audio_err_t err = esp_opus_dec_decode(stream->decoder, &raw, &frame, &info);

    if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && frame.needed_size > stream->dec_pcm_size) {
        uint8_t *new_buf = realloc(stream->dec_pcm_buf, frame.needed_size);
        if (!new_buf) {
            ESP_LOGE(TAG, "Voice stream decoder buffer realloc failed: %" PRIu32 " bytes", frame.needed_size);
            stream->rx_errors++;
            return ESP_ERR_NO_MEM;
        }
        stream->dec_pcm_buf = new_buf;
        stream->dec_pcm_size = frame.needed_size;
        frame.buffer = stream->dec_pcm_buf;
        frame.len = stream->dec_pcm_size;
        err = esp_opus_dec_decode(stream->decoder, &raw, &frame, &info);
    }

    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Voice stream Opus decode failed: %d", err);
        stream->rx_errors++;
        return ESP_FAIL;
    }
    if (frame.decoded_size > 0) {
        int ret = esp_codec_dev_write(stream->output->codec_dev, frame.buffer, (int)frame.decoded_size);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Voice stream output write failed: %d", ret);
            stream->rx_errors++;
            return ESP_FAIL;
        }
        stream->rx_frames++;
        stream->rx_bytes += frame.decoded_size;
    }
    return ESP_OK;
}

static void audio_voice_handle_binary(audio_voice_stream_t *stream, const esp_websocket_event_data_t *event)
{
    if (!stream || !event || !event->data_ptr || event->data_len <= 0) {
        return;
    }

    if (event->payload_len > event->data_len || event->payload_offset > 0) {
        if ((uint32_t)event->payload_len > VOICE_STREAM_MAX_FRAGMENT_BYTES) {
            ESP_LOGW(TAG, "Voice stream binary frame too large: %d", event->payload_len);
            stream->rx_errors++;
            return;
        }
        if (!stream->rx_frag_buf || stream->rx_frag_size != (uint32_t)event->payload_len) {
            free(stream->rx_frag_buf);
            stream->rx_frag_buf = malloc((size_t)event->payload_len);
            stream->rx_frag_size = (uint32_t)event->payload_len;
            stream->rx_frag_received = 0;
        }
        if (!stream->rx_frag_buf ||
            event->payload_offset < 0 ||
            event->data_len < 0 ||
            (uint32_t)(event->payload_offset + event->data_len) > stream->rx_frag_size) {
            ESP_LOGE(TAG, "Voice stream invalid websocket fragment");
            stream->rx_errors++;
            return;
        }
        memcpy(stream->rx_frag_buf + event->payload_offset, event->data_ptr, (size_t)event->data_len);
        stream->rx_frag_received += (uint32_t)event->data_len;
        if (stream->rx_frag_received >= stream->rx_frag_size) {
            audio_voice_decode_and_play(stream, stream->rx_frag_buf, stream->rx_frag_size);
            stream->rx_frag_received = 0;
        }
        return;
    }

    audio_voice_decode_and_play(stream, (const uint8_t *)event->data_ptr, (uint32_t)event->data_len);
}

static void audio_voice_ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    audio_voice_stream_t *stream = (audio_voice_stream_t *)handler_args;
    esp_websocket_event_data_t *event = (esp_websocket_event_data_t *)event_data;

    if (!stream) {
        return;
    }
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        stream->connected = true;
        ESP_LOGI(TAG, "Voice stream websocket connected");
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        stream->connected = false;
        ESP_LOGI(TAG, "Voice stream websocket disconnected");
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA || !event) {
        return;
    }

    if (event->op_code == WS_TRANSPORT_OPCODES_BINARY) {
        audio_voice_handle_binary(stream, event);
        return;
    }
    if (event->op_code == WS_TRANSPORT_OPCODES_TEXT && event->data_ptr && event->data_len > 0) {
        ESP_LOGI(TAG, "Voice stream control: %.*s", event->data_len, event->data_ptr);
        if (audio_voice_text_is_done(event->data_ptr, event->data_len)) {
            stream->server_done = true;
        }
    }
}

static esp_err_t audio_voice_init_codecs(audio_voice_stream_t *stream)
{
    esp_opus_enc_config_t enc_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    esp_opus_dec_cfg_t dec_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    esp_audio_err_t err;

    enc_cfg.sample_rate = (int)stream->sample_rate;
    enc_cfg.channel = stream->channels;
    enc_cfg.bits_per_sample = stream->bits;
    enc_cfg.bitrate = stream->bitrate;
    enc_cfg.frame_duration = audio_voice_enc_duration(stream->frame_ms);
    enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    enc_cfg.complexity = stream->complexity;
    enc_cfg.enable_vbr = stream->enable_vbr;
    enc_cfg.enable_dtx = stream->enable_dtx;
    if (enc_cfg.frame_duration == ESP_OPUS_ENC_FRAME_DURATION_ARG) {
        ESP_LOGE(TAG, "Voice stream unsupported frame_ms: %" PRIu32, stream->frame_ms);
        return ESP_ERR_INVALID_ARG;
    }
    err = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &stream->encoder);
    if (err != ESP_AUDIO_ERR_OK || !stream->encoder) {
        ESP_LOGE(TAG, "Voice stream Opus encoder open failed: %d", err);
        return ESP_FAIL;
    }
    err = esp_opus_enc_get_frame_size(stream->encoder, &stream->pcm_size, &stream->opus_size);
    if (err != ESP_AUDIO_ERR_OK || stream->pcm_size <= 0 || stream->opus_size <= 0) {
        ESP_LOGE(TAG, "Voice stream Opus frame size failed: %d", err);
        return ESP_FAIL;
    }

    dec_cfg.sample_rate = stream->sample_rate;
    dec_cfg.channel = stream->channels;
    dec_cfg.frame_duration = audio_voice_dec_duration(stream->frame_ms);
    dec_cfg.self_delimited = false;
    err = esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &stream->decoder);
    if (err != ESP_AUDIO_ERR_OK || !stream->decoder) {
        ESP_LOGE(TAG, "Voice stream Opus decoder open failed: %d", err);
        return ESP_FAIL;
    }

    stream->pcm_buf = malloc((size_t)stream->pcm_size);
    stream->opus_buf = malloc((size_t)stream->opus_size);
    stream->dec_pcm_size = (stream->sample_rate * stream->channels * stream->frame_ms * (stream->bits / 8U)) / 1000U;
    if (stream->dec_pcm_size < (uint32_t)stream->pcm_size) {
        stream->dec_pcm_size = (uint32_t)stream->pcm_size;
    }
    stream->dec_pcm_buf = malloc(stream->dec_pcm_size);
    if (!stream->pcm_buf || !stream->opus_buf || !stream->dec_pcm_buf) {
        ESP_LOGE(TAG, "Voice stream buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static char *audio_voice_build_headers(audio_voice_stream_t *stream)
{
    if (!stream->token || stream->token[0] == '\0') {
        return NULL;
    }
    size_t len = strlen(stream->token) + 32;
    char *headers = malloc(len);
    if (!headers) {
        return NULL;
    }
    snprintf(headers, len, "Authorization: Bearer %s\r\n", stream->token);
    return headers;
}

static esp_err_t audio_voice_send_control(audio_voice_stream_t *stream, const char *state)
{
    char msg[320];
    int len = snprintf(msg, sizeof(msg),
                       "{\"type\":\"listen\",\"state\":\"%s\",\"session_id\":\"%s\"}",
                       state,
                       stream->session_id ? stream->session_id : "esp-claw");
    if (len <= 0 || len >= (int)sizeof(msg)) {
        return ESP_FAIL;
    }
    int sent = esp_websocket_client_send_text(stream->ws, msg, len, pdMS_TO_TICKS(VOICE_STREAM_SEND_TIMEOUT_MS));
    return sent == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t audio_voice_send_hello(audio_voice_stream_t *stream)
{
    char msg[512];
    int len = snprintf(msg, sizeof(msg),
                       "{\"type\":\"hello\",\"transport\":\"websocket\","
                       "\"version\":1,\"session_id\":\"%s\","
                       "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%" PRIu32 ","
                       "\"channels\":%u,\"bits\":%u,\"frame_duration\":%" PRIu32 "}}",
                       stream->session_id ? stream->session_id : "esp-claw",
                       stream->sample_rate,
                       stream->channels,
                       stream->bits,
                       stream->frame_ms);
    if (len <= 0 || len >= (int)sizeof(msg)) {
        return ESP_FAIL;
    }
    int sent = esp_websocket_client_send_text(stream->ws, msg, len, pdMS_TO_TICKS(VOICE_STREAM_SEND_TIMEOUT_MS));
    return sent == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t audio_voice_wait_connected(audio_voice_stream_t *stream)
{
    int64_t start = esp_timer_get_time() / 1000LL;
    while (!stream->connected) {
        if ((esp_timer_get_time() / 1000LL) - start > VOICE_STREAM_CONNECT_TIMEOUT_MS) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return ESP_OK;
}

static esp_err_t audio_voice_validate_formats(audio_voice_stream_t *stream)
{
    if (stream->bits != 16 || stream->channels != 1) {
        ESP_LOGE(TAG, "Voice stream only supports 16-bit mono Opus now");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (stream->input->fmt.sample_rate != stream->sample_rate ||
        stream->input->fmt.channels != stream->channels ||
        stream->input->fmt.bits != stream->bits) {
        ESP_LOGE(TAG, "Voice input format mismatch: device=%" PRIu32 "/%u/%u stream=%" PRIu32 "/%u/%u",
                 stream->input->fmt.sample_rate, stream->input->fmt.channels, stream->input->fmt.bits,
                 stream->sample_rate, stream->channels, stream->bits);
        return ESP_ERR_INVALID_STATE;
    }
    if (stream->output->fmt.sample_rate != stream->sample_rate ||
        stream->output->fmt.channels != stream->channels ||
        stream->output->fmt.bits != stream->bits) {
        ESP_LOGE(TAG, "Voice output format mismatch: device=%" PRIu32 "/%u/%u stream=%" PRIu32 "/%u/%u",
                 stream->output->fmt.sample_rate, stream->output->fmt.channels, stream->output->fmt.bits,
                 stream->sample_rate, stream->channels, stream->bits);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

int lua_audio_voice_stream_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "input");
    audio_device_t *input = lua_audio_check_device(L, -1, AUDIO_DEVICE_INPUT, "voice_stream");
    lua_pop(L, 1);
    lua_getfield(L, 1, "output");
    audio_device_t *output = lua_audio_check_device(L, -1, AUDIO_DEVICE_OUTPUT, "voice_stream");
    lua_pop(L, 1);

    char *uri = audio_voice_get_string_field(L, 1, "uri", NULL);
    if (!uri) {
        return lua_audio_push_error(L, "audio voice_stream: uri is required");
    }

    audio_voice_stream_t *stream = (audio_voice_stream_t *)lua_newuserdata(L, sizeof(*stream));
    memset(stream, 0, sizeof(*stream));
    stream->input = input;
    stream->output = output;
    stream->uri = uri;
    stream->token = audio_voice_get_string_field(L, 1, "token", NULL);
    stream->session_id = audio_voice_get_string_field(L, 1, "session_id", "esp-claw");
    stream->sample_rate = lua_audio_get_u32_field(L, 1, "sample_rate", 0, 16000);
    stream->frame_ms = lua_audio_get_u32_field(L, 1, "frame_ms", 0, 60);
    stream->record_ms = lua_audio_get_u32_field(L, 1, "record_ms", 0, 5000);
    stream->playback_timeout_ms = lua_audio_get_u32_field(L, 1, "playback_timeout_ms", 0, 15000);
    stream->channels = lua_audio_get_u8_field(L, 1, "channels", 0, 1);
    stream->bits = lua_audio_get_u8_field(L, 1, "bits", 0, 16);
    stream->bitrate = lua_audio_get_int_field(L, 1, "bitrate", 24000);
    stream->complexity = lua_audio_get_int_field(L, 1, "complexity", 0);
    stream->enable_vbr = audio_voice_get_bool_field(L, 1, "vbr", true);
    stream->enable_dtx = audio_voice_get_bool_field(L, 1, "dtx", true);
    stream->input_ref = LUA_NOREF;
    stream->output_ref = LUA_NOREF;
    stream->closed = false;

    lua_getfield(L, 1, "input");
    stream->input_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_getfield(L, 1, "output");
    stream->output_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    input->holders++;
    output->holders++;

    luaL_getmetatable(L, AUDIO_VOICE_STREAM_META);
    lua_setmetatable(L, -2);
    return 1;
}

int lua_audio_voice_stream_run(lua_State *L)
{
    audio_voice_stream_t *stream = lua_audio_check_voice_stream(L, 1);
    uint32_t record_ms = stream->record_ms;
    uint32_t playback_timeout_ms = stream->playback_timeout_ms;
    char *headers = NULL;
    esp_err_t err;

    if (stream->running) {
        return lua_audio_push_error(L, "audio voice_stream: already running");
    }
    if (lua_istable(L, 2)) {
        record_ms = lua_audio_get_u32_field(L, 2, "record_ms", 0, record_ms);
        playback_timeout_ms = lua_audio_get_u32_field(L, 2, "playback_timeout_ms", 0, playback_timeout_ms);
    }

    err = audio_voice_validate_formats(stream);
    if (err != ESP_OK) {
        return lua_audio_push_error(L, "audio voice_stream: input/output must be 16kHz mono 16-bit");
    }
    if (!audio_device_acquire(stream->input)) {
        return lua_audio_push_error(L, "audio voice_stream: input busy");
    }
    if (!audio_device_acquire(stream->output)) {
        audio_device_release(stream->input);
        return lua_audio_push_error(L, "audio voice_stream: output busy");
    }

    stream->running = true;
    stream->connected = false;
    stream->server_done = false;
    stream->tx_frames = 0;
    stream->rx_frames = 0;
    stream->tx_bytes = 0;
    stream->rx_bytes = 0;
    stream->tx_errors = 0;
    stream->rx_errors = 0;

    err = audio_voice_init_codecs(stream);
    if (err == ESP_OK) {
        headers = audio_voice_build_headers(stream);
        esp_websocket_client_config_t ws_config = {
            .uri = stream->uri,
            .buffer_size = VOICE_STREAM_WS_BUFFER_SIZE,
            .task_stack = VOICE_STREAM_WS_TASK_STACK,
            .network_timeout_ms = 10000,
            .reconnect_timeout_ms = 1000,
            .disable_auto_reconnect = true,
            .headers = headers,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        stream->ws = esp_websocket_client_init(&ws_config);
        if (!stream->ws) {
            err = ESP_FAIL;
        } else {
            esp_websocket_register_events(stream->ws, WEBSOCKET_EVENT_ANY, audio_voice_ws_event_handler, stream);
            err = esp_websocket_client_start(stream->ws);
        }
    }
    if (err == ESP_OK) {
        err = audio_voice_wait_connected(stream);
    }
    if (err == ESP_OK) {
        err = audio_voice_send_hello(stream);
    }
    if (err == ESP_OK) {
        err = audio_voice_send_control(stream, "start");
    }

    if (err == ESP_OK) {
        int64_t end_ms = (esp_timer_get_time() / 1000LL) + record_ms;
        while (stream->connected && (esp_timer_get_time() / 1000LL) < end_ms) {
            int ret = esp_codec_dev_read(stream->input->codec_dev, stream->pcm_buf, stream->pcm_size);
            if (ret != ESP_CODEC_DEV_OK) {
                ESP_LOGE(TAG, "Voice stream input read failed: %d", ret);
                stream->tx_errors++;
                err = ESP_FAIL;
                break;
            }
            esp_audio_enc_in_frame_t in_frame = {
                .buffer = stream->pcm_buf,
                .len = (uint32_t)stream->pcm_size,
            };
            esp_audio_enc_out_frame_t out_frame = {
                .buffer = stream->opus_buf,
                .len = (uint32_t)stream->opus_size,
            };
            esp_audio_err_t enc_err = esp_opus_enc_process(stream->encoder, &in_frame, &out_frame);
            if (enc_err != ESP_AUDIO_ERR_OK || out_frame.encoded_bytes == 0) {
                ESP_LOGE(TAG, "Voice stream Opus encode failed: %d", enc_err);
                stream->tx_errors++;
                err = ESP_FAIL;
                break;
            }
            int sent = esp_websocket_client_send_bin(stream->ws,
                                                     (const char *)stream->opus_buf,
                                                     out_frame.encoded_bytes,
                                                     pdMS_TO_TICKS(VOICE_STREAM_SEND_TIMEOUT_MS));
            if (sent != (int)out_frame.encoded_bytes) {
                ESP_LOGE(TAG, "Voice stream send failed: sent=%d expected=%" PRIu32, sent, out_frame.encoded_bytes);
                stream->tx_errors++;
                err = ESP_FAIL;
                break;
            }
            stream->tx_frames++;
            stream->tx_bytes += out_frame.encoded_bytes;
        }
        audio_voice_send_control(stream, "stop");
    }

    if (err == ESP_OK && playback_timeout_ms > 0) {
        int64_t end_ms = (esp_timer_get_time() / 1000LL) + playback_timeout_ms;
        while (stream->connected && !stream->server_done && (esp_timer_get_time() / 1000LL) < end_ms) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    audio_voice_cleanup_runtime(stream);
    free(headers);
    audio_device_release(stream->output);
    audio_device_release(stream->input);
    stream->running = false;

    if (err != ESP_OK) {
        return lua_audio_push_errorf(L, "audio voice_stream: run failed (%s)", esp_err_to_name(err));
    }

    lua_newtable(L);
    lua_pushinteger(L, stream->tx_frames);
    lua_setfield(L, -2, "tx_frames");
    lua_pushinteger(L, stream->rx_frames);
    lua_setfield(L, -2, "rx_frames");
    lua_pushinteger(L, stream->tx_bytes);
    lua_setfield(L, -2, "tx_bytes");
    lua_pushinteger(L, stream->rx_bytes);
    lua_setfield(L, -2, "rx_bytes");
    lua_pushinteger(L, stream->tx_errors);
    lua_setfield(L, -2, "tx_errors");
    lua_pushinteger(L, stream->rx_errors);
    lua_setfield(L, -2, "rx_errors");
    lua_pushboolean(L, stream->server_done);
    lua_setfield(L, -2, "server_done");
    return 1;
}

int lua_audio_voice_stream_close(lua_State *L)
{
    audio_voice_stream_t *stream = (audio_voice_stream_t *)lua_touserdata(L, 1);
    if (!stream || stream->closed) {
        lua_pushboolean(L, 1);
        return 1;
    }
    if (stream->running) {
        return lua_audio_push_error(L, "audio voice_stream: running");
    }
    audio_voice_cleanup_runtime(stream);
    if (stream->input && stream->input->holders > 0) {
        stream->input->holders--;
    }
    if (stream->output && stream->output->holders > 0) {
        stream->output->holders--;
    }
    if (stream->input_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, stream->input_ref);
        stream->input_ref = LUA_NOREF;
    }
    if (stream->output_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, stream->output_ref);
        stream->output_ref = LUA_NOREF;
    }
    free(stream->uri);
    free(stream->token);
    free(stream->session_id);
    stream->uri = NULL;
    stream->token = NULL;
    stream->session_id = NULL;
    stream->closed = true;
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_voice_stream_gc(lua_State *L)
{
    return lua_audio_voice_stream_close(L);
}
