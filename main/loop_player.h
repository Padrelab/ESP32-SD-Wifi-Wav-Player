#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audio_sdcard.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define LOOP_PLAYER_STATUS_TEXT_LENGTH   96
#define LOOP_PLAYER_VOLUME_MAX_PERCENT   100U

typedef enum {
    LOOP_PLAYER_STATE_STARTING = 0,
    LOOP_PLAYER_STATE_IDLE,
    LOOP_PLAYER_STATE_PLAYING,
    LOOP_PLAYER_STATE_PAUSED,
    LOOP_PLAYER_STATE_RECOVERING,
    LOOP_PLAYER_STATE_WAITING_FOR_MEDIA,
    LOOP_PLAYER_STATE_ERROR,
} loop_player_state_t;

typedef struct {
    loop_player_state_t state;
    bool sd_ready;
    uint32_t sample_rate_hz;
    uint16_t channels;
    uint32_t loop_count;
    uint32_t volume_percent;
    uint32_t sd_remount_count;
    uint32_t sd_remount_failure_count;
    uint32_t sd_recovery_backoff_ms;
    char last_error[LOOP_PLAYER_STATUS_TEXT_LENGTH];
} loop_player_status_t;

esp_err_t loop_player_start(audio_sdcard_t *sdcard);
esp_err_t loop_player_begin_update(TickType_t timeout);
void loop_player_end_update(bool reload_requested);
void loop_player_get_status(loop_player_status_t *status);
uint32_t loop_player_get_volume_percent(void);
esp_err_t loop_player_set_volume_percent(uint32_t volume_percent);
const char *loop_player_state_name(loop_player_state_t state);
