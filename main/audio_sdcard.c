#include "audio_sdcard.h"

#include <dirent.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"
#include "sd_protocol_defs.h"

static const char *TAG = "audio_sdcard";
static const int AUDIO_SD_MAX_TRANSFER_SIZE = 32 * 1024;

static const char *audio_sdcard_card_type_name(const sdmmc_card_t *card)
{
    if (card == NULL) {
        return "unknown";
    }

    if (card->is_sdio) {
        return "sdio";
    }

    if (card->is_mmc) {
        return "mmc";
    }

    return (card->ocr & SD_OCR_SDHC_CAP) != 0 ? "sdhc/sdxc" : "sdsc";
}

static void audio_sdcard_log_card_info(const sdmmc_card_t *card)
{
    uint64_t capacity_mib = 0;
    float speed_mhz = 0.0f;

    if (card == NULL) {
        return;
    }

    capacity_mib = ((uint64_t) card->csd.capacity) * (uint64_t) card->csd.sector_size / (1024ULL * 1024ULL);
    speed_mhz = (float) card->max_freq_khz / 1000.0f;

    ESP_LOGI(
        TAG,
        "SD card ready: name=%s type=%s size=%lluMiB sector=%d speed=%.2fMHz",
        card->cid.name,
        audio_sdcard_card_type_name(card),
        (unsigned long long) capacity_mib,
        card->csd.sector_size,
        speed_mhz
    );
}

static bool audio_sdcard_filesystem_accessible_locked(audio_sdcard_t *sdcard)
{
    DIR *dir = NULL;

    if (sdcard == NULL || !sdcard->mounted || sdcard->card == NULL) {
        return false;
    }

    if (sdmmc_get_status(sdcard->card) != ESP_OK) {
        return false;
    }

    dir = opendir(CONFIG_STORAGE_MOUNT_POINT);
    if (dir == NULL) {
        return false;
    }

    closedir(dir);
    return true;
}

static void audio_sdcard_unmount_locked(audio_sdcard_t *sdcard)
{
    if (sdcard == NULL) {
        return;
    }

    if (sdcard->mounted && sdcard->card != NULL) {
        esp_vfs_fat_sdcard_unmount(CONFIG_STORAGE_MOUNT_POINT, sdcard->card);
    }

    spi_bus_free(sdcard->host_slot);
    sdcard->card = NULL;
    sdcard->mounted = false;
}

static esp_err_t audio_sdcard_init_bus(audio_sdcard_t *sdcard)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_SD_SPI_PIN_MOSI,
        .miso_io_num = CONFIG_SD_SPI_PIN_MISO,
        .sclk_io_num = CONFIG_SD_SPI_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = AUDIO_SD_MAX_TRANSFER_SIZE,
    };
    esp_err_t err = spi_bus_initialize(sdcard->host_slot, &bus_cfg, SPI_DMA_CH_AUTO);

    if (err == ESP_ERR_INVALID_STATE) {
        spi_bus_free(sdcard->host_slot);
        err = spi_bus_initialize(sdcard->host_slot, &bus_cfg, SPI_DMA_CH_AUTO);
    }

    return err;
}

const char *audio_sdcard_mount_point(void)
{
    return CONFIG_STORAGE_MOUNT_POINT;
}

esp_err_t audio_sdcard_init(audio_sdcard_t *sdcard)
{
    ESP_RETURN_ON_FALSE(sdcard != NULL, ESP_ERR_INVALID_ARG, TAG, "SD card state is required");

    memset(sdcard, 0, sizeof(*sdcard));
    sdcard->host_slot = SPI2_HOST;
    sdcard->mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(sdcard->mutex != NULL, ESP_ERR_NO_MEM, TAG, "Failed to allocate SD mutex");
    return ESP_OK;
}

void audio_sdcard_deinit(audio_sdcard_t *sdcard)
{
    if (sdcard == NULL || sdcard->mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(sdcard->mutex, portMAX_DELAY) == pdTRUE) {
        audio_sdcard_unmount_locked(sdcard);
        xSemaphoreGive(sdcard->mutex);
    }

    vSemaphoreDelete(sdcard->mutex);
    sdcard->mutex = NULL;
}

esp_err_t audio_sdcard_ensure_mounted(audio_sdcard_t *sdcard)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    esp_err_t err = ESP_OK;

    ESP_RETURN_ON_FALSE(sdcard != NULL, ESP_ERR_INVALID_ARG, TAG, "SD card state is required");
    ESP_RETURN_ON_FALSE(sdcard->mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "SD card state is not initialized");

    xSemaphoreTake(sdcard->mutex, portMAX_DELAY);

    if (audio_sdcard_filesystem_accessible_locked(sdcard)) {
        xSemaphoreGive(sdcard->mutex);
        return ESP_OK;
    }

    audio_sdcard_unmount_locked(sdcard);

    err = audio_sdcard_init_bus(sdcard);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(err));
        xSemaphoreGive(sdcard->mutex);
        return err;
    }

    host.slot = sdcard->host_slot;
    host.max_freq_khz = CONFIG_SD_SPI_FREQUENCY_KHZ;

    slot_config.host_id = sdcard->host_slot;
    slot_config.gpio_cs = CONFIG_SD_SPI_PIN_CS;

    ESP_LOGI(TAG, "Mounting SD card at %s over SPI", CONFIG_STORAGE_MOUNT_POINT);
    err = esp_vfs_fat_sdspi_mount(CONFIG_STORAGE_MOUNT_POINT, &host, &slot_config, &mount_config, &sdcard->card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(err));
        spi_bus_free(sdcard->host_slot);
        sdcard->card = NULL;
        sdcard->mounted = false;
        xSemaphoreGive(sdcard->mutex);
        return err;
    }

    sdcard->mounted = true;
    audio_sdcard_log_card_info(sdcard->card);
    xSemaphoreGive(sdcard->mutex);
    return ESP_OK;
}

bool audio_sdcard_is_ready(audio_sdcard_t *sdcard)
{
    bool ready = false;

    if (sdcard == NULL || sdcard->mutex == NULL) {
        return false;
    }

    xSemaphoreTake(sdcard->mutex, portMAX_DELAY);
    ready = audio_sdcard_filesystem_accessible_locked(sdcard);
    xSemaphoreGive(sdcard->mutex);
    return ready;
}
