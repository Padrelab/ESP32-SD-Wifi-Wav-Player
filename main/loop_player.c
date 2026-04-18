#include "loop_player.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "audio_output.h"
#include "audio_wav.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#define PLAYER_EVENT_IDLE     BIT0
#define PLAYER_EVENT_PAUSE    BIT1
#define PLAYER_EVENT_RELOAD   BIT2
#define PLAYER_FILE_BUFFER_SIZE_BYTES   (32 * 1024)
#define PLAYER_LATENCY_WARN_US          (75 * 1000)
#define PLAYER_MAX_LATENCY_WARNINGS     8
#define PLAYER_SD_REMOUNT_DELAY_MS      500
#define PLAYER_NVS_NAMESPACE            "player"
#define PLAYER_NVS_VOLUME_KEY           "volume"

typedef enum {
    LOOP_PLAYER_RECOVERY_NONE = 0,
    LOOP_PLAYER_RECOVERY_ACTIVE,
    LOOP_PLAYER_RECOVERY_WAITING_FOR_MEDIA,
} loop_player_recovery_mode_t;

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

static uint32_t loop_player_load_volume_percent(void)
{
    nvs_handle_t nvs = 0;
    uint8_t stored_volume = 0;
    esp_err_t err = nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &nvs);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return CONFIG_PLAYER_VOLUME_PERCENT;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for volume load: %s", esp_err_to_name(err));
        return CONFIG_PLAYER_VOLUME_PERCENT;
    }

    err = nvs_get_u8(nvs, PLAYER_NVS_VOLUME_KEY, &stored_volume);
    nvs_close(nvs);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return CONFIG_PLAYER_VOLUME_PERCENT;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load volume from NVS: %s", esp_err_to_name(err));
        return CONFIG_PLAYER_VOLUME_PERCENT;
    }

    if (stored_volume > LOOP_PLAYER_VOLUME_MAX_PERCENT) {
        ESP_LOGW(TAG, "Ignoring invalid stored volume: %u%%", stored_volume);
        return CONFIG_PLAYER_VOLUME_PERCENT;
    }

    return stored_volume;
}

static esp_err_t loop_player_save_volume_percent(uint32_t volume_percent)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs);

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(nvs, PLAYER_NVS_VOLUME_KEY, (uint8_t) volume_percent);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

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

static void loop_player_set_sd_recovery_metrics(
    loop_player_context_t *ctx,
    uint32_t remount_count,
    uint32_t remount_failure_count,
    uint32_t recovery_backoff_ms
)
{
    if (ctx == NULL || ctx->mutex == NULL) {
        return;
    }

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->status.sd_remount_count = remount_count;
    ctx->status.sd_remount_failure_count = remount_failure_count;
    ctx->status.sd_recovery_backoff_ms = recovery_backoff_ms;
    ctx->status.sd_ready = audio_sdcard_is_ready(ctx->sdcard);
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

static uint32_t loop_player_next_recovery_backoff_ms(uint32_t current_delay_ms)
{
    if (current_delay_ms == 0U) {
        return CONFIG_PLAYER_SD_RECOVERY_RETRY_DELAY_MS;
    }

    if (current_delay_ms >= CONFIG_PLAYER_SD_RECOVERY_MAX_RETRY_DELAY_MS) {
        return CONFIG_PLAYER_SD_RECOVERY_MAX_RETRY_DELAY_MS;
    }

    if (current_delay_ms > (CONFIG_PLAYER_SD_RECOVERY_MAX_RETRY_DELAY_MS / 2U)) {
        return CONFIG_PLAYER_SD_RECOVERY_MAX_RETRY_DELAY_MS;
    }

    return current_delay_ms * 2U;
}

static bool loop_player_should_log_recovery_failure(uint32_t consecutive_failures)
{
    if (consecutive_failures <= 3U) {
        return true;
    }

    return (consecutive_failures & (consecutive_failures - 1U)) == 0U;
}

static bool loop_player_should_wait_for_media(uint32_t consecutive_failures)
{
    if (CONFIG_PLAYER_SD_ACTIVE_RECOVERY_ATTEMPTS == 0) {
        return consecutive_failures > 0U;
    }

    return consecutive_failures >= CONFIG_PLAYER_SD_ACTIVE_RECOVERY_ATTEMPTS;
}

static uint32_t loop_player_recovery_sd_frequency_khz(uint32_t base_frequency_khz, uint32_t consecutive_failures)
{
    uint32_t downclock_frequency_khz = base_frequency_khz;
    uint32_t halving_steps = consecutive_failures;

    while (halving_steps > 0U && downclock_frequency_khz > CONFIG_PLAYER_SD_RECOVERY_MIN_FREQUENCY_KHZ) {
        downclock_frequency_khz /= 2U;
        halving_steps -= 1U;
    }

    if (downclock_frequency_khz < CONFIG_PLAYER_SD_RECOVERY_MIN_FREQUENCY_KHZ) {
        downclock_frequency_khz = CONFIG_PLAYER_SD_RECOVERY_MIN_FREQUENCY_KHZ;
    }

    if (downclock_frequency_khz > base_frequency_khz) {
        downclock_frequency_khz = base_frequency_khz;
    }

    return downclock_frequency_khz;
}

static esp_err_t loop_player_probe_sd_media_once(loop_player_context_t *ctx)
{
    struct stat st = {0};
    FILE *file = NULL;
    uint8_t probe_buffer[64];
    int stat_errno = 0;
    int read_errno = 0;
    size_t bytes_read = 0;
    esp_err_t err = audio_sdcard_ensure_mounted(ctx->sdcard);

    if (err != ESP_OK) {
        return err;
    }

    errno = 0;
    if (stat(CONFIG_PLAYER_WAV_PATH, &st) != 0) {
        stat_errno = errno;
        if (stat_errno == ENOENT) {
            return ESP_OK;
        }

        ESP_LOGW(
            TAG,
            "Media probe stat failed for %s: errno=%d (%s)",
            CONFIG_PLAYER_WAV_PATH,
            stat_errno,
            strerror(stat_errno)
        );
        return ESP_ERR_INVALID_RESPONSE;
    }

    errno = 0;
    file = fopen(CONFIG_PLAYER_WAV_PATH, "rb");
    if (file == NULL) {
        int open_errno = errno;

        if (open_errno == ENOENT) {
            return ESP_OK;
        }

        ESP_LOGW(
            TAG,
            "Media probe open failed for %s: errno=%d (%s)",
            CONFIG_PLAYER_WAV_PATH,
            open_errno,
            strerror(open_errno)
        );
        return ESP_ERR_INVALID_RESPONSE;
    }

    errno = 0;
    bytes_read = fread(probe_buffer, 1, sizeof(probe_buffer), file);
    read_errno = errno;

    if (ferror(file) != 0 || (bytes_read == 0 && !feof(file))) {
        ESP_LOGW(
            TAG,
            "Media probe read failed for %s: got=%u errno=%d (%s)",
            CONFIG_PLAYER_WAV_PATH,
            (unsigned) bytes_read,
            read_errno,
            read_errno != 0 ? strerror(read_errno) : "n/a"
        );
        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }

    fclose(file);
    return ESP_OK;
}

static esp_err_t loop_player_wait_for_sd_stability(loop_player_context_t *ctx)
{
    uint32_t stable_check = 0;

    if (CONFIG_PLAYER_SD_RETURN_SETTLE_DELAY_MS > 0) {
        ESP_LOGI(
            TAG,
            "Waiting %u ms for SD card settle before media probes",
            (unsigned) CONFIG_PLAYER_SD_RETURN_SETTLE_DELAY_MS
        );
        vTaskDelay(pdMS_TO_TICKS(CONFIG_PLAYER_SD_RETURN_SETTLE_DELAY_MS));
    }

    for (stable_check = 0; stable_check < CONFIG_PLAYER_SD_RETURN_STABLE_CHECKS; ++stable_check) {
        esp_err_t err = loop_player_probe_sd_media_once(ctx);

        if (err != ESP_OK) {
            return err;
        }

        if ((stable_check + 1U) < CONFIG_PLAYER_SD_RETURN_STABLE_CHECKS &&
            CONFIG_PLAYER_SD_RETURN_STABLE_CHECK_INTERVAL_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_PLAYER_SD_RETURN_STABLE_CHECK_INTERVAL_MS));
        }
    }

    return ESP_OK;
}

static bool loop_player_is_transient_sdcard_fault(loop_player_context_t *ctx, esp_err_t err)
{
    if (err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_TIMEOUT) {
        return true;
    }

    if (ctx == NULL || ctx->sdcard == NULL || audio_sdcard_is_ready(ctx->sdcard)) {
        return false;
    }

    return err != ESP_OK && err != ESP_ERR_INVALID_STATE;
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
        case LOOP_PLAYER_STATE_RECOVERING:
            return "recovering";
        case LOOP_PLAYER_STATE_WAITING_FOR_MEDIA:
            return "waiting_for_media";
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

uint32_t loop_player_get_volume_percent(void)
{
    uint32_t volume_percent = CONFIG_PLAYER_VOLUME_PERCENT;

    if (s_player.mutex == NULL) {
        return volume_percent;
    }

    xSemaphoreTake(s_player.mutex, portMAX_DELAY);
    volume_percent = s_player.status.volume_percent;
    xSemaphoreGive(s_player.mutex);

    return volume_percent;
}

esp_err_t loop_player_set_volume_percent(uint32_t volume_percent)
{
    esp_err_t err = ESP_OK;

    ESP_RETURN_ON_FALSE(
        volume_percent <= LOOP_PLAYER_VOLUME_MAX_PERCENT,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Invalid volume: %lu%%",
        (unsigned long) volume_percent
    );
    ESP_RETURN_ON_FALSE(s_player.mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "Player is not started");

    xSemaphoreTake(s_player.mutex, portMAX_DELAY);
    s_player.status.volume_percent = volume_percent;
    xSemaphoreGive(s_player.mutex);

    err = loop_player_save_volume_percent(volume_percent);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist volume %lu%%: %s", (unsigned long) volume_percent, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Volume set to %lu%%", (unsigned long) volume_percent);
    return ESP_OK;
}

static float loop_player_volume_scale(uint32_t volume_percent)
{
    return (float) volume_percent / 100.0f;
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
        int open_errno = errno;

        if (open_errno == ENOENT) {
            return ESP_ERR_NOT_FOUND;
        }

        ESP_LOGW(
            TAG,
            "Failed to open %s: errno=%d (%s)",
            CONFIG_PLAYER_WAV_PATH,
            open_errno,
            strerror(open_errno)
        );
        return ESP_ERR_INVALID_RESPONSE;
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
            int read_errno = errno;

            if (ferror(file) != 0 || !feof(file)) {
                ESP_LOGW(
                    TAG,
                    "Read failure on %s: requested=%u errno=%d (%s) remaining=%u",
                    CONFIG_PLAYER_WAV_PATH,
                    (unsigned) bytes_to_read,
                    read_errno,
                    read_errno != 0 ? strerror(read_errno) : "n/a",
                    (unsigned) remaining
                );
                err = ESP_ERR_INVALID_RESPONSE;
            } else {
                err = ESP_FAIL;
            }
            break;
        }

        available_bytes = buffered_bytes + bytes_read;
        aligned_bytes = available_bytes - (available_bytes % wav_info.block_align);

        if (aligned_bytes > 0) {
            int64_t write_start_us = 0;
            int64_t write_time_us = 0;
            uint32_t volume_percent = loop_player_get_volume_percent();

            if (wav_info.encoding == AUDIO_WAV_ENCODING_PCM) {
                i2s_bytes = audio_wav_convert_pcm_chunk_to_stereo_i32(
                    &wav_info,
                    read_buffer,
                    aligned_bytes,
                    volume_percent,
                    i2s_buffer
                );
            } else {
                size_t frames = audio_wav_convert_chunk_to_stereo_f32(&wav_info, read_buffer, aligned_bytes, sample_buffer);
                audio_output_apply_volume(sample_buffer, frames, loop_player_volume_scale(volume_percent));
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
            int read_errno = errno;

            if (ferror(file) != 0 || !feof(file)) {
                ESP_LOGW(
                    TAG,
                    "Short read failure on %s: got=%u requested=%u errno=%d (%s) remaining=%u",
                    CONFIG_PLAYER_WAV_PATH,
                    (unsigned) bytes_read,
                    (unsigned) bytes_to_read,
                    read_errno,
                    read_errno != 0 ? strerror(read_errno) : "n/a",
                    (unsigned) remaining
                );
                err = ESP_ERR_INVALID_RESPONSE;
            } else {
                err = ESP_FAIL;
            }
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
    uint32_t sd_remount_count = 0;
    uint32_t sd_remount_failure_count = 0;
    uint32_t sd_consecutive_remount_failures = 0;
    uint32_t sd_recovery_backoff_ms = CONFIG_PLAYER_SD_RECOVERY_RETRY_DELAY_MS;
    uint32_t sd_recovery_base_frequency_khz = CONFIG_SD_SPI_FREQUENCY_KHZ;
    loop_player_recovery_mode_t recovery_mode = LOOP_PLAYER_RECOVERY_NONE;

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
    loop_player_set_sd_recovery_metrics(ctx, sd_remount_count, sd_remount_failure_count, sd_recovery_backoff_ms);

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

        if (recovery_mode != LOOP_PLAYER_RECOVERY_NONE) {
            uint32_t wait_ms = recovery_mode == LOOP_PLAYER_RECOVERY_WAITING_FOR_MEDIA
                ? CONFIG_PLAYER_SD_WAIT_FOR_MEDIA_POLL_MS
                : (sd_consecutive_remount_failures > 0U ? sd_recovery_backoff_ms : 0U);
            const char *status_text = recovery_mode == LOOP_PLAYER_RECOVERY_WAITING_FOR_MEDIA
                ? "Waiting for SD card"
                : "Recovering SD card";

            audio_output_stop(&ctx->output);
            loop_player_set_status(
                ctx,
                recovery_mode == LOOP_PLAYER_RECOVERY_WAITING_FOR_MEDIA
                    ? LOOP_PLAYER_STATE_WAITING_FOR_MEDIA
                    : LOOP_PLAYER_STATE_RECOVERING,
                0,
                0,
                status_text
            );
            loop_player_set_sd_recovery_metrics(ctx, sd_remount_count, sd_remount_failure_count, sd_recovery_backoff_ms);

            if (wait_ms > 0U) {
                vTaskDelay(pdMS_TO_TICKS(wait_ms));
            }

            if ((xEventGroupGetBits(ctx->events) & PLAYER_EVENT_PAUSE) != 0) {
                continue;
            }

            if (sd_consecutive_remount_failures == 0U) {
                ESP_LOGW(TAG, "Attempting SD card remount/recovery");
            } else if (loop_player_should_log_recovery_failure(sd_consecutive_remount_failures + 1U)) {
                ESP_LOGI(
                    TAG,
                    "Attempting SD card remount/recovery (attempt=%lu, current_backoff_ms=%lu, spi=%.2fMHz)",
                    (unsigned long) (sd_consecutive_remount_failures + 1U),
                    (unsigned long) sd_recovery_backoff_ms,
                    (double) loop_player_recovery_sd_frequency_khz(
                        sd_recovery_base_frequency_khz,
                        sd_consecutive_remount_failures
                    ) / 1000.0
                );
            }

            audio_sdcard_set_frequency_khz(
                ctx->sdcard,
                loop_player_recovery_sd_frequency_khz(
                    sd_recovery_base_frequency_khz,
                    sd_consecutive_remount_failures
                )
            );
            err = audio_sdcard_force_remount(ctx->sdcard);
            if (err == ESP_OK) {
                err = loop_player_wait_for_sd_stability(ctx);
            }

            if (err == ESP_OK) {
                sd_remount_count++;
                if (sd_consecutive_remount_failures > 0U) {
                    ESP_LOGI(
                        TAG,
                        "SD card remount succeeded after %lu failed attempt(s) at %.2fMHz",
                        (unsigned long) sd_consecutive_remount_failures,
                        (double) audio_sdcard_get_frequency_khz(ctx->sdcard) / 1000.0
                    );
                }

                sd_recovery_base_frequency_khz = audio_sdcard_get_frequency_khz(ctx->sdcard);
                if (sd_recovery_base_frequency_khz < CONFIG_SD_SPI_FREQUENCY_KHZ) {
                    ESP_LOGI(
                        TAG,
                        "Latched safer SD SPI recovery base at %.2fMHz until reboot",
                        (double) sd_recovery_base_frequency_khz / 1000.0
                    );
                }

                sd_consecutive_remount_failures = 0;
                sd_recovery_backoff_ms = CONFIG_PLAYER_SD_RECOVERY_RETRY_DELAY_MS;
                recovery_mode = LOOP_PLAYER_RECOVERY_NONE;
                loop_player_set_sd_recovery_metrics(ctx, sd_remount_count, sd_remount_failure_count, sd_recovery_backoff_ms);
                continue;
            }

            sd_remount_failure_count++;
            sd_consecutive_remount_failures++;
            sd_recovery_backoff_ms = loop_player_next_recovery_backoff_ms(sd_recovery_backoff_ms);
            recovery_mode = loop_player_should_wait_for_media(sd_consecutive_remount_failures)
                ? LOOP_PLAYER_RECOVERY_WAITING_FOR_MEDIA
                : LOOP_PLAYER_RECOVERY_ACTIVE;
            loop_player_set_sd_recovery_metrics(ctx, sd_remount_count, sd_remount_failure_count, sd_recovery_backoff_ms);

            if (loop_player_should_log_recovery_failure(sd_consecutive_remount_failures)) {
                ESP_LOGW(
                    TAG,
                    "SD card remount failed (attempt=%lu, consecutive_failures=%lu, next_backoff_ms=%lu, spi=%.2fMHz): %s",
                    (unsigned long) sd_consecutive_remount_failures,
                    (unsigned long) sd_consecutive_remount_failures,
                    (unsigned long) sd_recovery_backoff_ms,
                    (double) audio_sdcard_get_frequency_khz(ctx->sdcard) / 1000.0,
                    esp_err_to_name(err)
                );
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

        if (loop_player_is_transient_sdcard_fault(ctx, err)) {
            ESP_LOGW(TAG, "Scheduling SD card recovery after transient fault: %s", esp_err_to_name(err));
            recovery_mode = LOOP_PLAYER_RECOVERY_ACTIVE;
            loop_player_set_status(ctx, LOOP_PLAYER_STATE_RECOVERING, 0, 0, "Recovering SD card");
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

    s_player.status.volume_percent = loop_player_load_volume_percent();
    ESP_LOGI(TAG, "Initial volume: %lu%%", (unsigned long) s_player.status.volume_percent);
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
