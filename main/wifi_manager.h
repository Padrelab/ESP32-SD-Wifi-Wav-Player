#pragma once

#include "esp_err.h"

esp_err_t wifi_manager_start_softap(void);
const char *wifi_manager_ap_ip(void);
