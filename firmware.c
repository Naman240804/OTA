#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define BLINK_GPIO 2
#define BLINK_DELAY_MS 1000

#define WIFI_SSID      "Airtel_yath_0430" // Your Wi-Fi SSID
#define WIFI_PASS  "Kirti@766"        // Your Wi-Fi password
#define VERSION_JSON_URL "http://github.com/Naman240804/OTA/blob/d13a818c2afc622e587352c237ba22db9ab6db30/firmware.json"
#define CURRENT_VERSION 1

static const char *TAG = "OTA_JSON";
static char firmware_url[256];

// Wi-Fi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Retrying Wi-Fi connection...");
    }
}

// Initialize Wi-Fi
static void wifi_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

// Check version and fetch firmware URL
static int check_version(void)
{
    esp_http_client_config_t client_config = {
        .url = VERSION_JSON_URL,
        .timeout_ms = 5000,
        .method = HTTP_METHOD_GET,
    };

    esp_http_client_handle_t client = esp_http_client_init(&client_config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return -1;
    }

    // Open connection
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }

    // Get content length
    int content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "Content-Length reported by server: %d", content_length);
    if (content_length <= 0 || content_length > 2048) {
        ESP_LOGE(TAG, "Invalid content length");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    char *buffer = malloc(content_length + 1);
    if (!buffer) {
        ESP_LOGE(TAG, "Memory allocation failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    int data_read = esp_http_client_read(client, buffer, content_length);
    if (data_read <= 0) {
        ESP_LOGE(TAG, "Failed to read HTTP response or no data (data_read=%d)", data_read);
        free(buffer);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }
    buffer[data_read] = '\0';  // Null-terminate

    ESP_LOGI(TAG, "Fetched JSON (%d bytes): %s", data_read, buffer);

    // Parse JSON
    cJSON *json = cJSON_Parse(buffer);
    free(buffer);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!json) {
        ESP_LOGE(TAG, "JSON parse error");
        return -1;
    }

    cJSON *version_item = cJSON_GetObjectItem(json, "version");
    cJSON *url_item = cJSON_GetObjectItem(json, "url");
    if (!version_item || !url_item || !cJSON_IsNumber(version_item) || !cJSON_IsString(url_item)) {
        cJSON_Delete(json);
        ESP_LOGE(TAG, "Invalid JSON structure");
        return -1;
    }

    int new_version = version_item->valueint;
    strncpy(firmware_url, url_item->valuestring, sizeof(firmware_url) - 1);
    firmware_url[sizeof(firmware_url) - 1] = '\0';
    cJSON_Delete(json);

    if (new_version > CURRENT_VERSION) {
        ESP_LOGI(TAG, "New version available: %d", new_version);
        return 1;  // update needed
    }

    ESP_LOGI(TAG, "Firmware up-to-date: %d", CURRENT_VERSION);
    return 0;  // no update
}




// OTA update task
void ota_update_task(void *pvParameter)
{
    int check = check_version();
    if (check != 1) {
        ESP_LOGI(TAG, "No OTA update required");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Starting OTA update task...");

    esp_http_client_config_t config = { .url = firmware_url };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        goto cleanup;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        goto cleanup;
    }

    // Fetch headers first to get content length
    int content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "HTTP Status = %d, content_length = %d",
             esp_http_client_get_status_code(client),
             content_length);

    if (content_length <= 0) {
        ESP_LOGE(TAG, "Content-Length invalid or zero, cannot download OTA");
        goto cleanup;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "OTA partition not found");
        goto cleanup;
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    int binary_file_length = 0;
    char ota_buf[1024];

    while (1) {
        int data_read = esp_http_client_read(client, ota_buf, sizeof(ota_buf));
        if (data_read < 0) {
            ESP_LOGE(TAG, "HTTP read error");
            esp_ota_abort(ota_handle);
            goto cleanup;
        } else if (data_read > 0) {
            err = esp_ota_write(ota_handle, ota_buf, data_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
                esp_ota_abort(ota_handle);
                goto cleanup;
            }
            binary_file_length += data_read;
            ESP_LOGI(TAG, "Read %d bytes, total %d", data_read, binary_file_length);
        } else {
            // Finished reading
            break;
        }
    }

    if (binary_file_length == 0) {
        ESP_LOGE(TAG, "OTA binary size is 0, aborting");
        esp_ota_abort(ota_handle);
        goto cleanup;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        esp_ota_abort(ota_handle);
        goto cleanup;
    }

    // Mark valid and reboot
    esp_ota_mark_app_valid_cancel_rollback();
    esp_ota_set_boot_partition(update_partition);
    ESP_LOGI(TAG, "OTA update successful, rebooting...");

    esp_restart();

cleanup:
    if (client) {
        esp_http_client_cleanup(client);
    }
    vTaskDelete(NULL);
}




void app_main(void)
{
    ESP_LOGI(TAG, "Initializing NVS...");
    nvs_flash_init();

    ESP_LOGI(TAG, "Initializing Wi-Fi...");
    wifi_init();

    // Wait for IP
    vTaskDelay(pdMS_TO_TICKS(5000));

    // Start OTA task
    xTaskCreate(&ota_update_task, "ota_update_task", 8192, NULL, 5, NULL);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    while (1) {
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(BLINK_DELAY_MS / portTICK_PERIOD_MS);
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(BLINK_DELAY_MS / portTICK_PERIOD_MS);
    }
    
}
