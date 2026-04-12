#include "audio_output.h"

#include <string.h>

#include "audio_pcm.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

#define AUDIO_I2S_TIMEOUT_MS   1000

static const char *TAG = "audio_output";

esp_err_t audio_output_init(audio_output_t *output)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    i2s_std_config_t std_cfg = {0};

    if (output == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(output, 0, sizeof(*output));

    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &output->tx_handle, NULL), TAG, "Failed to create I2S TX channel");

    std_cfg = (i2s_std_config_t) {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = CONFIG_I2S_MCLK_PIN < 0 ? I2S_GPIO_UNUSED : (gpio_num_t) CONFIG_I2S_MCLK_PIN,
            .bclk = (gpio_num_t) CONFIG_I2S_BCLK_PIN,
            .ws = (gpio_num_t) CONFIG_I2S_WS_PIN,
            .dout = (gpio_num_t) CONFIG_I2S_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = 0,
                .bclk_inv = 0,
                .ws_inv = 0,
            },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(output->tx_handle, &std_cfg), TAG, "Failed to init I2S std mode");
    return ESP_OK;
}

void audio_output_deinit(audio_output_t *output)
{
    if (output == NULL) {
        return;
    }

    audio_output_stop(output);
    if (output->tx_handle != NULL) {
        i2s_del_channel(output->tx_handle);
        output->tx_handle = NULL;
    }

    output->current_sample_rate_hz = 0;
}

esp_err_t audio_output_prepare(audio_output_t *output, uint32_t sample_rate_hz)
{
    if (output == NULL || output->tx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (output->tx_enabled) {
        ESP_RETURN_ON_ERROR(i2s_channel_disable(output->tx_handle), TAG, "Failed to disable I2S before reconfigure");
        output->tx_enabled = false;
    }

    if (output->current_sample_rate_hz != sample_rate_hz) {
        i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
        ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(output->tx_handle, &clk_cfg), TAG, "Failed to reconfigure I2S clock");
        output->current_sample_rate_hz = sample_rate_hz;
    }

    ESP_RETURN_ON_ERROR(i2s_channel_enable(output->tx_handle), TAG, "Failed to enable I2S TX");
    output->tx_enabled = true;
    return ESP_OK;
}

void audio_output_stop(audio_output_t *output)
{
    if (output != NULL && output->tx_enabled) {
        if (i2s_channel_disable(output->tx_handle) == ESP_OK) {
            output->tx_enabled = false;
        }
    }
}

void audio_output_apply_volume(float *samples, size_t frame_count, float volume_scale)
{
    audio_pcm_apply_volume(samples, frame_count, volume_scale);
}

size_t audio_output_convert_stereo_f32_to_i32(const float *input, size_t frame_count, int32_t *output)
{
    return audio_pcm_convert_stereo_f32_to_i32(input, frame_count, output);
}

esp_err_t audio_output_write_all(audio_output_t *output, const void *data, size_t total_bytes)
{
    const uint8_t *cursor = data;
    size_t remaining = total_bytes;

    if (output == NULL || output->tx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    while (remaining > 0) {
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(output->tx_handle, cursor, remaining, &bytes_written, AUDIO_I2S_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }

        cursor += bytes_written;
        remaining -= bytes_written;
    }

    return ESP_OK;
}
