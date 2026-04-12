#pragma once

#include "audio_sdcard.h"
#include "esp_err.h"

esp_err_t web_server_start(audio_sdcard_t *sdcard);
void web_server_stop(void);
