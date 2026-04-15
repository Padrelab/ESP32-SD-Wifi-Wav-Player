#include "loop_player.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_output.h"
#include "audio_wav.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define PLAYER_EVENT_IDLE     BIT0
#define PLAYER_EVENT_PAUSE    BIT1
#define PLAYER_EVENT_RELOAD   BIT2
#define PLAYER_FILE_BUFFER_SIZE_BYTES   (32 * 1024)
#define PLAYER_LATENCY_WARN_US          (75 * 1000)
#define PLAYER_MAX_LATENCY_WARNINGS     8

typedef struct {
    audio_sdcard_t *sdcard;
    audio_output_t output;
    EventGroupHandle_t events;
    SemaphoreHandle_t mutex;
    TaskHandle_t task;
    loop_player_status_t status;
} loop_player_context_t;

static const char *TAG = "loop_player";
static loop_player_context_t s_player = {0};

static void *loop_player_alloc_internal(size_t size, const char *label)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (ptr != NULL) {
        ESP_LOGI(TAG, "Allocated %s (%u bytes) in internal RAM", label, (unsigned) size);
    }

    return ptr;
}

static void loop_player_set_status_locked(
    loop_player_context_t *ctx,
    loop_player_state_t state,
    uint32_t sample_rate_hz,
    uint16_t channels,
    const char *error_text
)
{
    ctx->status.state = state;
    ctx->status.sample_rate_hz = sample_rate_hz;
    ctx->status.channels = channels;
    ctx->status.sd_ready = audio_sdcard_is_ready(ctx->sdcard);

    if (error_text != NULL) {
        strlcpy(ctx->status.last_error, error_text, sizeof(ctx->status.last_error));
    } else {
        ctx->status.last_error[0] = '\0';
    }
}

static void loop_player_set_status(
    loop_player_context_t *ctx,
    loop_player_state_t state,
    uint32_t sample_rate_hz,
    uint16_t channels,
    const char *error_text
)
{
    if (ctx == NULL || ctx->mutex == NULL) {
        return;
    }

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    loop_player_set_status_locked(ctx, state, sample_rate_hz, channels, error_text);
    xSemaphoreGive(ctx->mutex);
}

static void loop_player_set_errorf(loop_player_context_t *ctx, const char *fmt, ...)
{
    va_list args;
    char text[LOOP_PLAYER_STATUS_TEXT_LENGTH];

    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    loop_player_set_status(ctx, LOOP_PLAYER_STATE_ERROR, 0, 0, text);
}

const char *loop_player_state_name(loop_player_state_t state)
{
    switch (state) {
        case LOOP_PLAYER_STATE_STARTING:
            return "starting";
        case LOOP_PLAYER_STATE_IDLE:
            return "idle";
        case LOOP_PLAYER_STATE_PLAYING:
            return "playing";
        case LOOP_PLAYER_STATE_PAUSED:
            return "paused";
        case LOOP_PLAYER_STATE_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

void loop_player_get_status(loop_player_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));

    if (s_player.mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_player.mutex, portMAX_DELAY);
    *status = s_player.status;
    xSemaphoreGive(s_player.mutex);
}

static float loop_player_volume_scale(void)
{
    return (float) CONFIG_PLAYER_VOLUME_PERCENT / 100.0f;
}

static esp_err_t loop_player_play_once(
    loop_player_context_t *ctx,
    uint8_t *read_buffer,
    float *sample_buffer,
    int32_t *i2s_buffer
)
{
    FILE *file = NULL;
    uint8_t *file_buffer = NULL;
    audio_wav_info_t wav_info = {0};
    size_t remaining = 0;
    size_t buffered_bytes = 0;
    int64_t max_read_time_us = 0;
    int64_t max_write_time_us = 0;
    int latency_warning_count = 0;
    esp_err_t err = audio_sdcard_ensure_mounted(ctx->sdcard);

    if (err != ESP_OK) {
        return err;
    }

    file = fopen(CONFIG_PLAYER_WAV_PATH, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    // Keep the stdio cache in internal RAM so SD reads stay deterministic.
    file_buffer = loop_player_alloc_internal(PLAYER_FILE_BUFFER_SIZE_BYTES, "WAV stdio buffer");
    if (file_buffer != NULL) {
        if (setvbuf(file, (char *) file_buffer, _IOFBF, PLAYER_FILE_BUFFER_SIZE_BYTES) != 0) {
            ESP_LOGW(TAG, "Failed to attach internal stdio buffer to %s", CONFIG_PLAYER_WAV_PATH);
            free(file_buffer);
            file_buffer = NULL;
        }
    }

    err = audio_wav_parse_header(file, &wav_info);
    if (err != ESP_OK) {
        fclose(file);
        free(file_buffer);
        return err;
    }

    err = audio_output_prepare(&ctx->output, wav_info.sample_rate_hz);
    if (err != ESP_OK) {
        fclose(file);
        free(file_buffer);
        return err;
    }

    ESP_LOGI(
        TAG,
        "Playing %s: %lu Hz, %u channel(s), %u-bit %s, block_align=%u",
        CONFIG_PLAYER_WAV_PATH,
        (unsigned long) wav_info.sample_rate_hz,
        wav_info.channels,
        wav_info.valid_bits_per_sample,
        audio_wav_encoding_name(&wav_info),
        wav_info.block_align
    );

    loop_player_set_status(ctx, LOOP_PLAYER_STATE_PLAYING, wav_info.sample_rate_hz, wav_info.channels, NULL);
    remaining = wav_info.data_size_bytes;

    while (remaining > 0) {
        EventBits_t control = xEventGroupGetBits(ctx->events);
        size_t max_bytes_to_read = CONFIG_PLAYER_READ_BUFFER_SIZE - buffered_bytes;
        size_t bytes_to_read = remaining < max_bytes_to_read ? remaining : max_bytes_to_read;
        size_t bytes_read = 0;
        size_t available_bytes = 0;
        size_t aligned_bytes = 0;
        size_t i2s_bytes = 0;
        int64_t read_start_us = 0;
        int64_t read_time_us = 0;

        if ((control & (PLAYER_EVENT_PAUSE | PLAYER_EVENT_RELOAD)) != 0) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        read_start_us = esp_timer_get_time();
        bytes_read = fread(read_buffer + buffered_bytes, 1, bytes_to_read, file);
        read_time_us = esp_timer_get_time() - read_start_us;
        if (read_time_us > max_read_time_us) {
            max_read_time_us = read_time_us;
        }

        if (read_time_us >= PLAYER_LATENCY_WARN_US && latency_warning_count < PLAYER_MAX_LATENCY_WARNINGS) {
            ESP_LOGW(
                TAG,
                "Slow SD read: %lld us for %u bytes (remaining=%u, buffered=%u)",
                (long long) read_time_us,
                (unsigned) bytes_to_read,
                (unsigned) remaining,
                (unsigned) buffered_bytes
            );
            latency_warning_count += 1;
        }

        if (bytes_read == 0) {
            err = ferror(file) ? ESP_FAIL : ESP_OK;
            break;
        }

        available_bytes = buffered_bytes + bytes_read;
        aligned_bytes = available_bytes - (available_bytes % wav_info.block_align);

        if (aligned_bytes > 0) {
            int64_t write_start_us = 0;
            int64_t write_time_us = 0;

            if (wav_info.encoding == AUDIO_WAV_ENCODING_PCM) {
                i2s_bytes = audio_wav_convert_pcm_chunk_to_stereo_i32(
                    &wav_info,
                    read_buffer,
                    aligned_bytes,
                    CONFIG_PLAYER_VOLUME_PERCENT,
                    i2s_buffer
                );
            } else {
                size_t frames = audio_wav_convert_chunk_to_stereo_f32(&wav_info, read_buffer, aligned_bytes, sample_buffer);
                audio_output_apply_volume(sample_buffer, frames, loop_player_volume_scale());
                i2s_bytes = audio_output_convert_stereo_f32_to_i32(sample_buffer, frames, i2s_buffer);
            }

            write_start_us = esp_timer_get_time();
            err = audio_output_write_all(&ctx->output, i2s_buffer, i2s_bytes);
            write_time_us = esp_timer_get_time() - write_start_us;
            if (write_time_us > max_write_time_us) {
                max_write_time_us = write_time_us;
            }

            if (write_time_us >= PLAYER_LATENCY_WARN_US && latency_warning_count < PLAYER_MAX_LATENCY_WARNINGS) {
                ESP_LOGW(
                    TAG,
                    "Slow I2S write: %lld us for %u bytes (%u frames)",
                    (long long) write_time_us,
                    (unsigned) i2s_bytes,
                    (unsigned) (aligned_bytes / wav_info.block_align)
                );
                latency_warning_count += 1;
            }

            if (err != ESP_OK) {
                break;
            }
        }

        buffered_bytes = available_bytes - aligned_bytes;
        if (buffered_bytes > 0) {
            memmove(read_buffer, read_buffer + aligned_bytes, buffered_bytes);
        }

        remaining -= bytes_read;

        if (bytes_read < bytes_to_read) {
            err = ferror(file) ? ESP_FAIL : ESP_OK;
            break;
        }
    }

    if (err == ESP_OK && buffered_bytes != 0) {
        ESP_LOGE(TAG, "WAV stream ended with %u unaligned byte(s) left in the read buffer", (unsigned) buffered_bytes);
        err = ESP_ERR_INVALID_SIZE;
    }

    fclose(file);
    free(file_buffer);
    ESP_LOGI(
        TAG,
        "Playback timing for %s: max_read=%lld us max_write=%lld us",
        CONFIG_PLAYER_WAV_PATH,
        (long long) max_read_time_us,
        (long long) max_write_time_us
    );
    audio_output_stop(&ctx->output);
    return err;
}

static void loop_player_task(void *arg)
{
    loop_player_context_t *ctx = arg;
    uint8_t *read_buffer = NULL;
    float *sample_buffer = NULL;
    int32_t *i2s_buffer = NULL;
    size_t max_frame_count = CONFIG_PLAYER_READ_BUFFER_SIZE / 2;

    read_buffer = loop_player_alloc_internal(CONFIG_PLAYER_READ_BUFFER_SIZE, "WAV read buffer");
    sample_buffer = loop_player_alloc_internal(max_frame_count * 2 * sizeof(float), "sample buffer");
    i2s_buffer = loop_player_alloc_internal(max_frame_count * 2 * sizeof(int32_t), "I2S write buffer");

    if (read_buffer == NULL || sample_buffer == NULL || i2s_buffer == NULL) {
        loop_player_set_errorf(ctx, "Out of memory");
        free(read_buffer);
        free(sample_buffer);
        free(i2s_buffer);
        vTaskDelete(NULL);
        return;
    }

    xEventGroupSetBits(ctx->events, PLAYER_EVENT_IDLE);

    while (true) {
        EventBits_t control = xEventGroupGetBits(ctx->events);
        esp_err_t err = ESP_OK;

        if ((control & PLAYER_EVENT_PAUSE) != 0) {
            audio_output_stop(&ctx->output);
            xEventGroupSetBits(ctx->events, PLAYER_EVENT_IDLE);
            loop_player_set_status(ctx, LOOP_PLAYER_STATE_PAUSED, 0, 0, NULL);

            while ((xEventGroupGetBits(ctx->events) & PLAYER_EVENT_PAUSE) != 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            continue;
        }

        xEventGroupClearBits(ctx->events, PLAYER_EVENT_IDLE | PLAYER_EVENT_RELOAD);
        err = loop_player_play_once(ctx, read_buffer, sample_buffer, i2s_buffer);
        xEventGroupSetBits(ctx->events, PLAYER_EVENT_IDLE);
        control = xEventGroupGetBits(ctx->events);

        if ((control & PLAYER_EVENT_PAUSE) != 0) {
            continue;
        }

        if (err == ESP_ERR_INVALID_STATE) {
            continue;
        }

        if (err == ESP_OK) {
            xSemaphoreTake(ctx->mutex, portMAX_DELAY);
            ctx->status.loop_count += 1;
            ctx->status.sd_ready = audio_sdcard_is_ready(ctx->sdcard);
            xSemaphoreGive(ctx->mutex);
            continue;
        }

        if (err == ESP_ERR_NOT_FOUND) {
            loop_player_set_errorf(ctx, "Missing %s", CONFIG_PLAYER_WAV_PATH);
        } else {
            loop_player_set_errorf(ctx, "%s", esp_err_to_name(err));
        }

        ESP_LOGW(TAG, "Playback iteration failed: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(CONFIG_PLAYER_RETRY_DELAY_MS));
    }
}

esp_err_t loop_player_start(audio_sdcard_t *sdcard)
{
    esp_err_t err = ESP_OK;

    ESP_RETURN_ON_FALSE(sdcard != NULL, ESP_ERR_INVALID_ARG, TAG, "SD card state is required");
    ESP_RETURN_ON_FALSE(s_player.task == NULL, ESP_ERR_INVALID_STATE, TAG, "Player already started");

    memset(&s_player, 0, sizeof(s_player));
    s_player.sdcard = sdcard;
    s_player.events = xEventGroupCreate();
    s_player.mutex = xSemaphoreCreateMutex();

    ESP_RETURN_ON_FALSE(s_player.events != NULL, ESP_ERR_NO_MEM, TAG, "Failed to allocate event group");
    ESP_RETURN_ON_FALSE(s_player.mutex != NULL, ESP_ERR_NO_MEM, TAG, "Failed to allocate status mutex");

    loop_player_set_status(&s_player, LOOP_PLAYER_STATE_STARTING, 0, 0, NULL);

    err = audio_output_init(&s_player.output);
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to initialize audio output");

    BaseType_t task_ok = pdFAIL;

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    task_ok = xTaskCreatePinnedToCore(
        loop_player_task,
        "loop_player",
        CONFIG_PLAYER_TASK_STACK_SIZE,
        &s_player,
        CONFIG_PLAYER_TASK_PRIORITY,
        &s_player.task,
        1
    );
#else
    task_ok = xTaskCreate(
        loop_player_task,
        "loop_player",
        CONFIG_PLAYER_TASK_STACK_SIZE,
        &s_player,
        CONFIG_PLAYER_TASK_PRIORITY,
        &s_player.task
    );
#endif

    ESP_RETURN_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create playback task");
    return ESP_OK;
}

esp_err_t loop_player_begin_update(TickType_t timeout)
{
    EventBits_t bits = 0;

    ESP_RETURN_ON_FALSE(s_player.events != NULL, ESP_ERR_INVALID_STATE, TAG, "Player is not started");

    xEventGroupSetBits(s_player.events, PLAYER_EVENT_PAUSE | PLAYER_EVENT_RELOAD);
    bits = xEventGroupWaitBits(s_player.events, PLAYER_EVENT_IDLE, pdFALSE, pdFALSE, timeout);

    if ((bits & PLAYER_EVENT_IDLE) == 0) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

void loop_player_end_update(bool reload_requested)
{
    if (s_player.events == NULL) {
        return;
    }

    xEventGroupClearBits(s_player.events, PLAYER_EVENT_PAUSE | PLAYER_EVENT_RELOAD);

    if (reload_requested) {
        xEventGroupSetBits(s_player.events, PLAYER_EVENT_RELOAD);
    }
}
