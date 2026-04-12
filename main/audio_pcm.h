#pragma once

#include <stddef.h>
#include <stdint.h>

void audio_pcm_apply_volume(float *samples, size_t frame_count, float volume_scale);
size_t audio_pcm_convert_stereo_f32_to_i32(const float *input, size_t frame_count, int32_t *output);
