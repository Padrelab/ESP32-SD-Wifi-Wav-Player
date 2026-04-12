#include "loop_player.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_output.h"
#include "audio_wav.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define PLAYER_EVENT_IDLE     BIT0
#define PLAYER_EVENT_PAUSE    BIT1
#define PLAYER_EVENT_RELOAD   BIT2

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
    audio_wav_info_t wav_info = {0};
    size_t remaining = 0;
    esp_err_t err = audio_sdcard_ensure_mounted(ctx->sdcard);

    if (err != ESP_OK) {
        return err;
    }

    file = fopen(CONFIG_PLAYER_WAV_PATH, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    err = audio_wav_parse_header(file, &wav_info);
    if (err != ESP_OK) {
        fclose(file);
        return err;
    }

    err = audio_output_prepare(&ctx->output, wav_info.sample_rate_hz);
    if (err != ESP_OK) {
        fclose(file);
        return err;
    }

    loop_player_set_status(ctx, LOOP_PLAYER_STATE_PLAYING, wav_info.sample_rate_hz, wav_info.channels, NULL);
    remaining = wav_info.data_size_bytes;

    while (remaining > 0) {
        EventBits_t control = xEventGroupGetBits(ctx->events);
        size_t bytes_to_read = remaining < CONFIG_PLAYER_READ_BUFFER_SIZE ? remaining : CONFIG_PLAYER_READ_BUFFER_SIZE;
        size_t bytes_read = 0;
        size_t frames = 0;
        size_t i2s_bytes = 0;

        if ((control & (PLAYER_EVENT_PAUSE | PLAYER_EVENT_RELOAD)) != 0) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        bytes_read = fread(read_buffer, 1, bytes_to_read, file);
        if (bytes_read == 0) {
            err = ferror(file) ? ESP_FAIL : ESP_OK;
            break;
        }

        frames = audio_wav_convert_chunk_to_stereo_f32(&wav_info, read_buffer, bytes_read, sample_buffer);
        audio_output_apply_volume(sample_buffer, frames, loop_player_volume_scale());
        i2s_bytes = audio_output_convert_stereo_f32_to_i32(sample_buffer, frames, i2s_buffer);

        err = audio_output_write_all(&ctx->output, i2s_buffer, i2s_bytes);
        if (err != ESP_OK) {
            break;
        }

        remaining -= bytes_read;

        if (bytes_read < bytes_to_read) {
            err = ferror(file) ? ESP_FAIL : ESP_OK;
            break;
        }
    }

    fclose(file);
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

    read_buffer = malloc(CONFIG_PLAYER_READ_BUFFER_SIZE);
    sample_buffer = malloc(max_frame_count * 2 * sizeof(float));
    i2s_buffer = malloc(max_frame_count * 2 * sizeof(int32_t));

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

    BaseType_t task_ok = xTaskCreate(
        loop_player_task,
        "loop_player",
        CONFIG_PLAYER_TASK_STACK_SIZE,
        &s_player,
        CONFIG_PLAYER_TASK_PRIORITY,
        &s_player.task
    );

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
