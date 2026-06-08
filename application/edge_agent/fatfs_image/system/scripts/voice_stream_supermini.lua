local audio = require("audio")
local board_manager = require("board_manager")

local uri = args and args.uri or "ws://192.168.1.10:8080/ws/voice"
local record_ms = tonumber(args and args.record_ms) or 5000
local playback_timeout_ms = tonumber(args and args.playback_timeout_ms) or 15000

local in_codec, in_rate, in_channels, in_bits =
    board_manager.get_audio_codec_input_params("audio_adc")
local out_codec, out_rate, out_channels, out_bits =
    board_manager.get_audio_codec_output_params("audio_dac")

local input = assert(audio.new_input({ in_codec, in_rate, in_channels, in_bits, volume = 80 }))
local output = assert(audio.new_output({ out_codec, out_rate, out_channels, out_bits, volume = 80 }))
local stream = assert(audio.voice_stream({
    input = input,
    output = output,
    uri = uri,
    session_id = "esp-claw-supermini",
    sample_rate = 16000,
    channels = 1,
    bits = 16,
    frame_ms = 60,
    bitrate = 24000,
    complexity = 0,
    vbr = true,
    dtx = true,
}))

local ok, result = pcall(function()
    return stream:run({
        record_ms = record_ms,
        playback_timeout_ms = playback_timeout_ms,
    })
end)

stream:close()
input:close()
output:close()

if not ok then
    error(result)
end

print(string.format(
    "voice stream tx=%d frames/%d bytes rx=%d frames/%d bytes errors=%d/%d done=%s",
    result.tx_frames or 0,
    result.tx_bytes or 0,
    result.rx_frames or 0,
    result.rx_bytes or 0,
    result.tx_errors or 0,
    result.rx_errors or 0,
    tostring(result.server_done)
))
