#include "audio_wav.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

#define WAV_FORMAT_PCM          0x0001
#define WAV_FORMAT_IEEE_FLOAT   0x0003
#define WAV_FORMAT_EXTENSIBLE   0xFFFE

static const char *TAG = "audio_wav";
static const uint8_t WAV_EXTENSIBLE_GUID_SUFFIX[14] = {
    0x00, 0x00,
    0x00, 0x00,
    0x10, 0x00,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71,
};

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t) data[0] | ((uint16_t) data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t) data[0]
           | ((uint32_t) data[1] << 8)
           | ((uint32_t) data[2] << 16)
           | ((uint32_t) data[3] << 24);
}

static int32_t read_pcm24_le(const uint8_t *data)
{
    uint32_t sample = (uint32_t) data[0]
                      | ((uint32_t) data[1] << 8)
                      | ((uint32_t) data[2] << 16);

    if ((sample & 0x00800000U) != 0) {
        sample |= 0xFF000000U;
    }

    return (int32_t) sample;
}

static int32_t read_pcm32_le(const uint8_t *data)
{
    return (int32_t) read_le32(data);
}

const char *audio_wav_encoding_name(const audio_wav_info_t *info)
{
    return info->encoding == AUDIO_WAV_ENCODING_FLOAT ? "float" : "PCM";
}

static bool is_extensible_subformat(const uint8_t *guid, uint16_t format_tag)
{
    return read_le16(guid) == format_tag
           && memcmp(&guid[2], WAV_EXTENSIBLE_GUID_SUFFIX, sizeof(WAV_EXTENSIBLE_GUID_SUFFIX)) == 0;
}

static esp_err_t resolve_wav_format(uint16_t format_tag, uint16_t bits_per_sample, audio_wav_info_t *info)
{
    if (bits_per_sample % 8 != 0) {
        ESP_LOGE(TAG, "Sample container must be byte-aligned, bits_per_sample=%u", bits_per_sample);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (format_tag == WAV_FORMAT_PCM) {
        if (bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32) {
            ESP_LOGE(TAG, "Only PCM 16/24/32-bit WAV is supported, bits_per_sample=%u", bits_per_sample);
            return ESP_ERR_NOT_SUPPORTED;
        }

        info->encoding = AUDIO_WAV_ENCODING_PCM;
        return ESP_OK;
    }

    if (format_tag == WAV_FORMAT_IEEE_FLOAT) {
        if (bits_per_sample != 32) {
            ESP_LOGE(TAG, "Only 32-bit IEEE float WAV is supported, bits_per_sample=%u", bits_per_sample);
            return ESP_ERR_NOT_SUPPORTED;
        }

        info->encoding = AUDIO_WAV_ENCODING_FLOAT;
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Unsupported WAV format tag: %u", format_tag);
    return ESP_ERR_NOT_SUPPORTED;
}

static int32_t normalize_pcm_sample(int32_t sample, uint16_t container_bits, uint16_t valid_bits)
{
    if (valid_bits < container_bits) {
        sample = (int32_t) ((int64_t) sample / (int64_t) (1ULL << (container_bits - valid_bits)));
    }

    return sample;
}

static float pcm_to_f32(int32_t sample, uint16_t valid_bits)
{
    if (valid_bits == 0 || valid_bits > 32) {
        return 0.0f;
    }

    return (float) sample / (float) (1ULL << (valid_bits - 1));
}

static float float32_to_f32(const uint8_t *data)
{
    uint32_t raw = read_le32(data);
    float sample = 0.0f;

    memcpy(&sample, &raw, sizeof(sample));

    if (sample != sample) {
        return 0.0f;
    }

    return sample;
}

static float decode_wav_sample_to_f32(const audio_wav_info_t *info, const uint8_t *data)
{
    if (info->encoding == AUDIO_WAV_ENCODING_FLOAT) {
        return float32_to_f32(data);
    }

    int32_t sample = 0;

    if (info->bits_per_sample == 16) {
        sample = (int16_t) read_le16(data);
    } else if (info->bits_per_sample == 24) {
        sample = read_pcm24_le(data);
    } else {
        sample = read_pcm32_le(data);
    }

    sample = normalize_pcm_sample(sample, info->bits_per_sample, info->valid_bits_per_sample);
    return pcm_to_f32(sample, info->valid_bits_per_sample);
}

static int32_t decode_wav_sample_to_pcm_i32(const audio_wav_info_t *info, const uint8_t *data)
{
    int32_t sample = 0;

    if (info->bits_per_sample == 16) {
        sample = (int16_t) read_le16(data);
    } else if (info->bits_per_sample == 24) {
        sample = read_pcm24_le(data);
    } else {
        sample = read_pcm32_le(data);
    }

    sample = normalize_pcm_sample(sample, info->bits_per_sample, info->valid_bits_per_sample);
    return sample;
}

static int32_t scale_and_left_align_pcm_sample(int32_t sample, uint16_t valid_bits, uint32_t volume_percent)
{
    int shift = 32 - (int) valid_bits;
    int64_t aligned = (int64_t) sample << shift;
    int64_t scaled = (aligned * (int64_t) volume_percent) / 100;

    if (scaled > INT32_MAX) {
        return INT32_MAX;
    }

    if (scaled < INT32_MIN) {
        return INT32_MIN;
    }

    return (int32_t) scaled;
}

size_t audio_wav_convert_chunk_to_stereo_f32(
    const audio_wav_info_t *info,
    const uint8_t *input,
    size_t input_bytes,
    float *output
)
{
    size_t bytes_per_sample = info->bits_per_sample / 8;
    size_t frame_count = input_bytes / info->block_align;

    for (size_t frame = 0; frame < frame_count; ++frame) {
        const uint8_t *cursor = &input[frame * info->block_align];
        float left = decode_wav_sample_to_f32(info, cursor);
        float right = left;

        if (info->channels == 2) {
            right = decode_wav_sample_to_f32(info, cursor + bytes_per_sample);
        }

        output[2 * frame] = left;
        output[2 * frame + 1] = right;
    }

    return frame_count;
}

size_t audio_wav_convert_pcm_chunk_to_stereo_i32(
    const audio_wav_info_t *info,
    const uint8_t *input,
    size_t input_bytes,
    uint32_t volume_percent,
    int32_t *output
)
{
    size_t bytes_per_sample = info->bits_per_sample / 8;
    size_t frame_count = input_bytes / info->block_align;

    for (size_t frame = 0; frame < frame_count; ++frame) {
        const uint8_t *cursor = &input[frame * info->block_align];
        int32_t left = decode_wav_sample_to_pcm_i32(info, cursor);
        int32_t right = left;

        if (info->channels == 2) {
            right = decode_wav_sample_to_pcm_i32(info, cursor + bytes_per_sample);
        }

        output[2 * frame] = scale_and_left_align_pcm_sample(left, info->valid_bits_per_sample, volume_percent);
        output[2 * frame + 1] = scale_and_left_align_pcm_sample(right, info->valid_bits_per_sample, volume_percent);
    }

    return frame_count * 2 * sizeof(output[0]);
}

static esp_err_t audio_wav_read_exact(audio_storage_stream_t *stream, void *buffer, size_t bytes_to_read)
{
    if (audio_storage_stream_read(stream, buffer, bytes_to_read) != bytes_to_read) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t skip_bytes(audio_storage_stream_t *stream, uint32_t byte_count)
{
    if (audio_storage_stream_seek(stream, (long) byte_count, SEEK_CUR) != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t audio_wav_parse_header_stream(audio_storage_stream_t *stream, audio_wav_info_t *info)
{
    uint8_t riff_header[12];
    uint8_t chunk_header[8];
    bool fmt_found = false;
    bool data_found = false;

    ESP_RETURN_ON_FALSE(stream != NULL, ESP_ERR_INVALID_ARG, TAG, "WAV stream is required");
    ESP_RETURN_ON_FALSE(info != NULL, ESP_ERR_INVALID_ARG, TAG, "WAV info output is required");
    ESP_RETURN_ON_ERROR(audio_wav_read_exact(stream, riff_header, sizeof(riff_header)), TAG, "Failed to read RIFF header");

    if (memcmp(riff_header, "RIFF", 4) != 0 || memcmp(&riff_header[8], "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "File is not a RIFF/WAVE container");
        return ESP_ERR_NOT_SUPPORTED;
    }

    memset(info, 0, sizeof(*info));

    while (!data_found) {
        ESP_RETURN_ON_ERROR(audio_wav_read_exact(stream, chunk_header, sizeof(chunk_header)), TAG, "Failed to read WAV chunk header");

        uint32_t chunk_size = read_le32(&chunk_header[4]);
        uint32_t padded_chunk_size = chunk_size + (chunk_size & 1U);

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            uint8_t fmt_chunk[40] = {0};
            size_t fmt_bytes_to_read = chunk_size < sizeof(fmt_chunk) ? chunk_size : sizeof(fmt_chunk);

            if (chunk_size < 16 || audio_wav_read_exact(stream, fmt_chunk, fmt_bytes_to_read) != ESP_OK) {
                return ESP_FAIL;
            }

            uint16_t audio_format = read_le16(&fmt_chunk[0]);
            info->channels = read_le16(&fmt_chunk[2]);
            info->sample_rate_hz = read_le32(&fmt_chunk[4]);
            info->block_align = read_le16(&fmt_chunk[12]);
            info->bits_per_sample = read_le16(&fmt_chunk[14]);
            info->valid_bits_per_sample = info->bits_per_sample;

            if (audio_format == WAV_FORMAT_EXTENSIBLE) {
                if (chunk_size < 40 || read_le16(&fmt_chunk[16]) < 22) {
                    ESP_LOGE(TAG, "Incomplete WAVE_FORMAT_EXTENSIBLE fmt chunk");
                    return ESP_ERR_NOT_SUPPORTED;
                }

                if (is_extensible_subformat(&fmt_chunk[24], WAV_FORMAT_PCM)) {
                    audio_format = WAV_FORMAT_PCM;
                } else if (is_extensible_subformat(&fmt_chunk[24], WAV_FORMAT_IEEE_FLOAT)) {
                    audio_format = WAV_FORMAT_IEEE_FLOAT;
                } else {
                    ESP_LOGE(TAG, "Unsupported WAVE_FORMAT_EXTENSIBLE subformat");
                    return ESP_ERR_NOT_SUPPORTED;
                }

                info->valid_bits_per_sample = read_le16(&fmt_chunk[18]);
            }

            info->format_tag = audio_format;

            if (info->sample_rate_hz < AUDIO_WAV_MIN_SAMPLE_RATE_HZ || info->sample_rate_hz > AUDIO_WAV_MAX_SAMPLE_RATE_HZ) {
                ESP_LOGE(TAG, "Unsupported WAV sample rate: %lu Hz", (unsigned long) info->sample_rate_hz);
                return ESP_ERR_NOT_SUPPORTED;
            }

            if (info->valid_bits_per_sample == 0 || info->valid_bits_per_sample > info->bits_per_sample) {
                ESP_LOGE(
                    TAG,
                    "Invalid WAV valid bits setting: valid=%u container=%u",
                    info->valid_bits_per_sample,
                    info->bits_per_sample
                );
                return ESP_ERR_NOT_SUPPORTED;
            }

            if (info->channels != 1 && info->channels != 2) {
                ESP_LOGE(TAG, "Only mono or stereo WAV is supported, channels=%u", info->channels);
                return ESP_ERR_NOT_SUPPORTED;
            }

            if (info->block_align != info->channels * (info->bits_per_sample / 8)) {
                ESP_LOGE(
                    TAG,
                    "Unexpected block alignment, channels=%u bits=%u block_align=%u",
                    info->channels,
                    info->bits_per_sample,
                    info->block_align
                );
                return ESP_ERR_NOT_SUPPORTED;
            }

            ESP_RETURN_ON_ERROR(resolve_wav_format(audio_format, info->bits_per_sample, info), TAG, "Unsupported WAV format");

            fmt_found = true;

            if (padded_chunk_size > fmt_bytes_to_read) {
                ESP_RETURN_ON_ERROR(skip_bytes(stream, padded_chunk_size - fmt_bytes_to_read), TAG, "Failed to skip fmt tail");
            }
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            if (!fmt_found) {
                ESP_LOGE(TAG, "Encountered data chunk before fmt chunk");
                return ESP_ERR_INVALID_STATE;
            }

            info->data_size_bytes = chunk_size;
            data_found = true;
        } else {
            ESP_RETURN_ON_ERROR(skip_bytes(stream, padded_chunk_size), TAG, "Failed to skip WAV chunk");
        }
    }

    if (info->data_size_bytes == 0) {
        ESP_LOGE(TAG, "WAV data chunk is empty");
        return ESP_ERR_INVALID_SIZE;
    }

    if ((info->data_size_bytes % info->block_align) != 0) {
        ESP_LOGE(
            TAG,
            "WAV data size is not aligned to sample frames: data=%lu block_align=%u",
            (unsigned long) info->data_size_bytes,
            info->block_align
        );
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t audio_wav_parse_header(FILE *file, audio_wav_info_t *info)
{
    audio_storage_stream_t stream = {0};

    ESP_RETURN_ON_FALSE(file != NULL, ESP_ERR_INVALID_ARG, TAG, "Input file is required");
    audio_storage_stream_init_file(&stream, file);
    return audio_wav_parse_header_stream(&stream, info);
}
