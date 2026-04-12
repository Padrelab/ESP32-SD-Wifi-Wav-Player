#include "web_server.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio_wav.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/task.h"
#include "loop_player.h"
#include "sdkconfig.h"

#define UPLOAD_BUFFER_SIZE   2048

typedef struct {
    httpd_handle_t server;
    audio_sdcard_t *sdcard;
} web_server_context_t;

static const char *TAG = "web_server";
static web_server_context_t s_web = {0};

static const char k_index_html[] =
    "<!DOCTYPE html>"
    "<html lang='en'>"
    "<head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 WAV Player</title>"
    "<style>"
    "body{font-family:Verdana,sans-serif;background:#f4efe6;color:#202020;margin:0;padding:24px;}"
    "main{max-width:760px;margin:0 auto;display:grid;gap:16px;}"
    ".card{background:#fff;border-radius:16px;padding:20px;box-shadow:0 10px 30px rgba(0,0,0,.08);}"
    "h1{margin:0 0 8px;font-size:32px;}h2{margin:0 0 12px;font-size:20px;}"
    "p{line-height:1.5;margin:8px 0 0;}pre{background:#1f2937;color:#f9fafb;padding:16px;border-radius:12px;overflow:auto;}"
    "input,button{font:inherit;}input[type=file]{display:block;margin:12px 0;}"
    "button{background:#c2551a;color:#fff;border:0;border-radius:999px;padding:10px 16px;cursor:pointer;}"
    "button:hover{background:#9d4516;}.msg{margin-top:12px;white-space:pre-wrap;}"
    "code{background:#f1e4d4;padding:2px 6px;border-radius:6px;}"
    "</style>"
    "</head>"
    "<body>"
    "<main>"
    "<section class='card'>"
    "<h1>ESP32 WAV Player</h1>"
    "<p>Upload a new <code>.wav</code> file to replace the current loop on the SD card, or upload a fresh firmware <code>.bin</code> for OTA update.</p>"
    "</section>"
    "<section class='card'>"
    "<h2>Status</h2>"
    "<pre id='status'>Loading...</pre>"
    "<button onclick='refreshStatus()'>Refresh</button>"
    "</section>"
    "<section class='card'>"
    "<h2>Replace WAV</h2>"
    "<input id='wavFile' type='file' accept='.wav,audio/wav'>"
    "<button onclick='uploadFile(\"wav\")'>Upload WAV</button>"
    "<div id='wavMsg' class='msg'></div>"
    "</section>"
    "<section class='card'>"
    "<h2>OTA Firmware</h2>"
    "<input id='otaFile' type='file' accept='.bin,application/octet-stream'>"
    "<button onclick='uploadFile(\"ota\")'>Upload Firmware</button>"
    "<div id='otaMsg' class='msg'></div>"
    "</section>"
    "</main>"
    "<script>"
    "async function refreshStatus(){const r=await fetch('/api/status');document.getElementById('status').textContent=await r.text();}"
    "async function uploadFile(kind){"
    "const input=document.getElementById(kind==='wav'?'wavFile':'otaFile');"
    "const msg=document.getElementById(kind==='wav'?'wavMsg':'otaMsg');"
    "if(!input.files.length){msg.textContent='Choose a file first.';return;}"
    "const file=input.files[0];"
    "msg.textContent='Uploading '+file.name+' ...';"
    "try{"
    "const r=await fetch(kind==='wav'?'/api/upload/wav':'/api/upload/ota',{method:'POST',headers:{'Content-Type':'application/octet-stream','X-File-Name':file.name},body:file});"
    "msg.textContent=await r.text();"
    "}catch(e){msg.textContent='Upload failed: '+e;}"
    "refreshStatus();"
    "}"
    "refreshStatus();setInterval(refreshStatus,3000);"
    "</script>"
    "</body>"
    "</html>";

static void web_server_reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, k_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char body[512];
    loop_player_status_t status = {0};
    const esp_app_desc_t *app_desc = esp_app_get_description();

    loop_player_get_status(&status);

    snprintf(
        body,
        sizeof(body),
        "project: %s\nversion: %s\nstate: %s\nsd_ready: %s\nwav_path: %s\nsample_rate_hz: %lu\nchannels: %u\nloop_count: %lu\nlast_error: %s\n",
        app_desc->project_name,
        app_desc->version,
        loop_player_state_name(status.state),
        status.sd_ready ? "yes" : "no",
        CONFIG_PLAYER_WAV_PATH,
        (unsigned long) status.sample_rate_hz,
        status.channels,
        (unsigned long) status.loop_count,
        status.last_error[0] != '\0' ? status.last_error : "-"
    );

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t receive_body_to_file(httpd_req_t *req, const char *path)
{
    FILE *file = NULL;
    char buffer[UPLOAD_BUFFER_SIZE];
    int remaining = req->content_len;

    file = fopen(path, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    while (remaining > 0) {
        int chunk_size = remaining < (int) sizeof(buffer) ? remaining : (int) sizeof(buffer);
        int received = httpd_req_recv(req, buffer, chunk_size);

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }

        if (received <= 0) {
            fclose(file);
            unlink(path);
            return ESP_FAIL;
        }

        if (fwrite(buffer, 1, (size_t) received, file) != (size_t) received) {
            fclose(file);
            unlink(path);
            return ESP_FAIL;
        }

        remaining -= received;
    }

    fclose(file);
    return ESP_OK;
}

static esp_err_t wav_upload_post_handler(httpd_req_t *req)
{
    char temp_path[256];
    FILE *file = NULL;
    audio_wav_info_t wav_info = {0};
    esp_err_t err = ESP_OK;
    int rename_ok = -1;
    int unlink_ok = 0;
    char response[192];

    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request body");
    }

    snprintf(temp_path, sizeof(temp_path), "%s.upload", CONFIG_PLAYER_WAV_PATH);

    err = loop_player_begin_update(pdMS_TO_TICKS(CONFIG_PLAYER_UPDATE_TIMEOUT_MS));
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Player did not stop in time");
    }

    err = audio_sdcard_ensure_mounted(s_web.sdcard);
    if (err != ESP_OK) {
        loop_player_end_update(false);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card is not ready");
    }

    err = receive_body_to_file(req, temp_path);
    if (err != ESP_OK) {
        loop_player_end_update(false);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to store uploaded WAV");
    }

    file = fopen(temp_path, "rb");
    if (file == NULL) {
        unlink(temp_path);
        loop_player_end_update(false);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to reopen uploaded WAV");
    }

    err = audio_wav_parse_header(file, &wav_info);
    fclose(file);
    if (err != ESP_OK) {
        unlink(temp_path);
        loop_player_end_update(false);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Uploaded file is not a supported WAV");
    }

    unlink_ok = unlink(CONFIG_PLAYER_WAV_PATH);
    if (unlink_ok < 0 && access(CONFIG_PLAYER_WAV_PATH, F_OK) == 0) {
        unlink(temp_path);
        loop_player_end_update(false);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to remove previous WAV");
    }

    rename_ok = rename(temp_path, CONFIG_PLAYER_WAV_PATH);
    if (rename_ok != 0) {
        unlink(temp_path);
        loop_player_end_update(false);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to activate uploaded WAV");
    }

    loop_player_end_update(true);
    snprintf(
        response,
        sizeof(response),
        "WAV updated: %s, %lu Hz, %u channel(s).",
        CONFIG_PLAYER_WAV_PATH,
        (unsigned long) wav_info.sample_rate_hz,
        wav_info.channels
    );

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t ota_upload_post_handler(httpd_req_t *req)
{
    char buffer[UPLOAD_BUFFER_SIZE];
    int remaining = req->content_len;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = NULL;
    esp_err_t err = ESP_OK;

    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request body");
    }

    err = loop_player_begin_update(pdMS_TO_TICKS(CONFIG_PLAYER_UPDATE_TIMEOUT_MS));
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Player did not stop in time");
    }

    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        loop_player_end_update(false);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition available");
    }

    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        loop_player_end_update(false);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
    }

    while (remaining > 0) {
        int chunk_size = remaining < (int) sizeof(buffer) ? remaining : (int) sizeof(buffer);
        int received = httpd_req_recv(req, buffer, chunk_size);

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }

        if (received <= 0) {
            esp_ota_abort(ota_handle);
            loop_player_end_update(false);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA upload interrupted");
        }

        err = esp_ota_write(ota_handle, buffer, (size_t) received);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            loop_player_end_update(false);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_write failed");
        }

        remaining -= received;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        loop_player_end_update(false);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_end failed");
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        loop_player_end_update(false);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to select new boot partition");
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "OTA image stored. The board will reboot in a moment.");
    xTaskCreate(web_server_reboot_task, "ota_reboot", 2048, NULL, 4, NULL);
    return ESP_OK;
}

esp_err_t web_server_start(audio_sdcard_t *sdcard)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
    };
    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    httpd_uri_t wav_upload_uri = {
        .uri = "/api/upload/wav",
        .method = HTTP_POST,
        .handler = wav_upload_post_handler,
    };
    httpd_uri_t ota_upload_uri = {
        .uri = "/api/upload/ota",
        .method = HTTP_POST,
        .handler = ota_upload_post_handler,
    };

    ESP_RETURN_ON_FALSE(sdcard != NULL, ESP_ERR_INVALID_ARG, TAG, "SD card state is required");
    ESP_RETURN_ON_FALSE(s_web.server == NULL, ESP_ERR_INVALID_STATE, TAG, "Web server already started");

    s_web.sdcard = sdcard;
    config.max_uri_handlers = 8;
    config.stack_size = 6144;

    ESP_RETURN_ON_ERROR(httpd_start(&s_web.server, &config), TAG, "Failed to start HTTP server");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_web.server, &index_uri), TAG, "Failed to register /");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_web.server, &status_uri), TAG, "Failed to register /api/status");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_web.server, &wav_upload_uri), TAG, "Failed to register /api/upload/wav");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_web.server, &ota_upload_uri), TAG, "Failed to register /api/upload/ota");

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_web.server != NULL) {
        httpd_stop(s_web.server);
        s_web.server = NULL;
    }
}
