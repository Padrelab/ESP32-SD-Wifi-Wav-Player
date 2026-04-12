#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2s_common.h"
#include "esp_err.h"

typedef struct {
    i2s_chan_handle_t tx_handle;
    bool tx_enabled;
    uint32_t current_sample_rate_hz;
} audio_output_t;

esp_err_t audio_output_init(audio_output_t *output);
void audio_output_deinit(audio_output_t *output);
esp_err_t audio_output_prepare(audio_output_t *output, uint32_t sample_rate_hz);
void audio_output_stop(audio_output_t *output);
void audio_output_apply_volume(float *samples, size_t frame_count, float volume_scale);
size_t audio_output_convert_stereo_f32_to_i32(const float *input, size_t frame_count, int32_t *output);
esp_err_t audio_output_write_all(audio_output_t *output, const void *data, size_t total_bytes);
