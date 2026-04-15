#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "audio_storage_stream.h"
#include "esp_err.h"

#define AUDIO_WAV_MIN_SAMPLE_RATE_HZ   8000U
#define AUDIO_WAV_MAX_SAMPLE_RATE_HZ   192000U
#define AUDIO_WAV_MAX_BLOCK_ALIGN      8U

typedef enum {
    AUDIO_WAV_ENCODING_PCM = 0,
    AUDIO_WAV_ENCODING_FLOAT = 1,
} audio_wav_encoding_t;

typedef struct {
    uint32_t sample_rate_hz;
    uint16_t channels;
    uint16_t format_tag;
    uint16_t bits_per_sample;
    uint16_t valid_bits_per_sample;
    uint32_t data_size_bytes;
    uint16_t block_align;
    audio_wav_encoding_t encoding;
} audio_wav_info_t;

esp_err_t audio_wav_parse_header_stream(audio_storage_stream_t *stream, audio_wav_info_t *info);
esp_err_t audio_wav_parse_header(FILE *file, audio_wav_info_t *info);
const char *audio_wav_encoding_name(const audio_wav_info_t *info);
size_t audio_wav_convert_chunk_to_stereo_f32(
    const audio_wav_info_t *info,
    const uint8_t *input,
    size_t input_bytes,
    float *output
);
size_t audio_wav_convert_pcm_chunk_to_stereo_i32(
    const audio_wav_info_t *info,
    const uint8_t *input,
    size_t input_bytes,
    uint32_t volume_percent,
    int32_t *output
);
