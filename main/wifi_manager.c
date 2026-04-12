#include "wifi_manager.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_manager";

const char *wifi_manager_ap_ip(void)
{
    return "http://192.168.4.1/";
}

esp_err_t wifi_manager_start_softap(void)
{
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config = {0};
    wifi_auth_mode_t auth_mode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Failed to init netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Failed to create default event loop");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_ap() != NULL, ESP_FAIL, TAG, "Failed to create SoftAP netif");
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "Failed to initialize Wi-Fi");

    strlcpy((char *) wifi_config.ap.ssid, CONFIG_WIFI_AP_SSID, sizeof(wifi_config.ap.ssid));
    strlcpy((char *) wifi_config.ap.password, CONFIG_WIFI_AP_PASSWORD, sizeof(wifi_config.ap.password));
    wifi_config.ap.ssid_len = strlen(CONFIG_WIFI_AP_SSID);
    wifi_config.ap.channel = CONFIG_WIFI_AP_CHANNEL;
    wifi_config.ap.max_connection = CONFIG_WIFI_AP_MAX_CONNECTIONS;
    wifi_config.ap.pmf_cfg.required = false;

    if (strlen(CONFIG_WIFI_AP_PASSWORD) >= 8) {
        auth_mode = WIFI_AUTH_WPA2_PSK;
    }

    wifi_config.ap.authmode = auth_mode;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "Failed to set Wi-Fi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG, "Failed to configure SoftAP");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start Wi-Fi");

    ESP_LOGI(
        TAG,
        "SoftAP started: ssid=%s channel=%d auth=%s url=%s",
        CONFIG_WIFI_AP_SSID,
        CONFIG_WIFI_AP_CHANNEL,
        auth_mode == WIFI_AUTH_OPEN ? "open" : "wpa2",
        wifi_manager_ap_ip()
    );
    return ESP_OK;
}
