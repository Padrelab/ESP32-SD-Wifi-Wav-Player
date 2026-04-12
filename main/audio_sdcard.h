#pragma once

#include <stdbool.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

typedef struct {
    sdmmc_card_t *card;
    SemaphoreHandle_t mutex;
    bool mounted;
    spi_host_device_t host_slot;
} audio_sdcard_t;

esp_err_t audio_sdcard_init(audio_sdcard_t *sdcard);
void audio_sdcard_deinit(audio_sdcard_t *sdcard);
esp_err_t audio_sdcard_ensure_mounted(audio_sdcard_t *sdcard);
bool audio_sdcard_is_ready(audio_sdcard_t *sdcard);
const char *audio_sdcard_mount_point(void);
