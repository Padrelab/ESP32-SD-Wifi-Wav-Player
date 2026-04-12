#include "audio_pcm.h"

#include <limits.h>

static int32_t audio_pcm_float_to_i32(float sample)
{
    if (sample != sample) {
        return 0;
    }

    if (sample >= 1.0f) {
        return INT32_MAX;
    }

    if (sample <= -1.0f) {
        return INT32_MIN;
    }

    sample *= 2147483647.0f;
    sample += sample >= 0.0f ? 0.5f : -0.5f;
    return (int32_t) sample;
}

void audio_pcm_apply_volume(float *samples, size_t frame_count, float volume_scale)
{
    if (samples == NULL || volume_scale == 1.0f) {
        return;
    }

    size_t sample_count = frame_count * 2;

    for (size_t sample_index = 0; sample_index < sample_count; ++sample_index) {
        samples[sample_index] *= volume_scale;
    }
}

size_t audio_pcm_convert_stereo_f32_to_i32(const float *input, size_t frame_count, int32_t *output)
{
    size_t sample_count = frame_count * 2;

    for (size_t sample_index = 0; sample_index < sample_count; ++sample_index) {
        output[sample_index] = audio_pcm_float_to_i32(input[sample_index]);
    }

    return sample_count * sizeof(output[0]);
}
