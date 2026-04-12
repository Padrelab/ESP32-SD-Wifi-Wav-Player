#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "audio_sdcard.h"
#include "loop_player.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "app_main";
static audio_sdcard_t s_sdcard = {0};

static esp_err_t app_main_init_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Failed to erase NVS");
        err = nvs_flash_init();
    }

    return err;
}

void app_main(void)
{
    char elf_sha256[65] = {0};
    const esp_app_desc_t *app_desc = esp_app_get_description();
    esp_err_t err = ESP_OK;

    esp_app_get_elf_sha256(elf_sha256, sizeof(elf_sha256));
    ESP_LOGI(
        TAG,
        "Firmware identity: project=%s version=%s built=%s %s idf=%s elf_sha256=%s",
        app_desc->project_name,
        app_desc->version,
        app_desc->date,
        app_desc->time,
        app_desc->idf_ver,
        elf_sha256
    );

    err = app_main_init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    err = audio_sdcard_init(&s_sdcard);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_sdcard_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = audio_sdcard_ensure_mounted(&s_sdcard);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Initial SD mount failed: %s", esp_err_to_name(err));
    }

    err = loop_player_start(&s_sdcard);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "loop_player_start failed: %s", esp_err_to_name(err));
        return;
    }

    err = wifi_manager_start_softap();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_manager_start_softap failed: %s", esp_err_to_name(err));
        return;
    }

    err = web_server_start(&s_sdcard);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web_server_start failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Ready. Connect to SSID '%s' and open %s", CONFIG_WIFI_AP_SSID, wifi_manager_ap_ip());
}
