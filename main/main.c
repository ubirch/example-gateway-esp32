/*!
 * @file    main.c
 * @brief   main function of the example program
 *
 * @author Waldemar Gruenwald
 * @date   2018-10-10
 *
 * @copyright &copy; 2018 ubirch GmbH (https://ubirch.com)
 *
 * ```
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * ```
 */

//#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/ringbuf.h>
#include <esp_log.h>
#include <networking.h>
#include <sntp_time.h>
#include <ubirch_console.h>
#include <nvs_flash.h>
#include <ubirch_ota_task.h>
#include <ubirch_ota.h>
#include <time.h>

#include "storage.h"
#include "keys.h"
#include "id_handling.h"
#include "key_handling.h"
#include "token_handling.h"
#include "api-http-helper.h"
#include "register_thing.h"
#include "anchor.h"

char *TAG = "example-gateway";

// define task handles for every task
static TaskHandle_t fw_update_task_handle = NULL;
static TaskHandle_t net_config_handle = NULL;
static TaskHandle_t main_task_handle = NULL;
static TaskHandle_t sensor_simulator_task_handle = NULL;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-noreturn"

/*!
 * Simulation data
 */
typedef struct {
    unsigned char id;
    int32_t data;
} sensor_data_t;

static int32_t dummy_data = 0;

/*!
 * Sensor simulator task simulates incoming sensor data.
 */
static void sensor_simulator_task(void *pvParameters) {
    RingbufHandle_t buf_handle = *(RingbufHandle_t*)pvParameters;

    // TODO: at startup create a set of sensor-id's, or use a fix set
    unsigned char sensors[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};
    size_t number_of_sensors = ((sizeof sensors) / (sizeof *sensors));

    EventBits_t event_bits;

    // give system some time to start up
    vTaskDelay(pdMS_TO_TICKS(6000));

    // loop through the sensors
    for (size_t sensor_index = 0;; sensor_index = (sensor_index + 1) % number_of_sensors) {
        // check if network connection is up
        event_bits = xEventGroupWaitBits(network_event_group, WIFI_CONNECTED_BIT | NETWORK_ETH_READY,
                                         false, false, portMAX_DELAY);
        if ((event_bits & WIFI_CONNECTED_BIT) != WIFI_CONNECTED_BIT) {
            ESP_LOGW(TAG, "network not ready");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "Simulate sensor data from sensor %x", sensors[sensor_index]);
        sensor_data_t data = {
            .id = sensors[sensor_index],
            .data = dummy_data++
        };

        // send it to main task
        UBaseType_t res =  xRingbufferSend(buf_handle, &data, sizeof(sensor_data_t),
                pdMS_TO_TICKS(1000));
        if (res != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send sensor data");
        }

        vTaskDelay(pdMS_TO_TICKS(6000));
    }
}

/*!
 * Main task performs the main functionality of the application,
 * when the network is set up.
 *
 * In this example we randomly generate a device id and register it in
 * the ubirch backend. After that we simulate incomming data from this
 * device, we sign this data and and communicate with the backend as usual.
 * TODO: description
 *
 * @param pvParameters are currently not used, but part of the task declaration.
 */
static void main_task(void *pvParameters) {
    RingbufHandle_t buf_handle = *(RingbufHandle_t*)pvParameters;
    // load backend key
    if (load_backend_key() != ESP_OK) {
        ESP_LOGW(TAG, "unable to load backend key");
    }

    // load token
    if (ubirch_token_load() != ESP_OK) {
        ESP_LOGE(TAG, "failed to load token");
    }


    // load gateway-uuid
    uuid_t gateway_uuid;
    esp_efuse_mac_get_default(gateway_uuid);
    esp_base_mac_addr_set(gateway_uuid);
    gateway_uuid[15]++;

    // uuid string, to be reused later
    char gateway_uuid_string[37];
    uuid_to_string(gateway_uuid, gateway_uuid_string, sizeof(gateway_uuid_string));
    ESP_LOGI(TAG, "gateway uuid: %s", gateway_uuid_string);

    EventBits_t event_bits;

    for (;;) {
        // check if network connection is up
        event_bits = xEventGroupWaitBits(network_event_group, WIFI_CONNECTED_BIT | NETWORK_ETH_READY,
                                         false, false, portMAX_DELAY);
        if ((event_bits & WIFI_CONNECTED_BIT) != WIFI_CONNECTED_BIT) {
            ESP_LOGW(TAG, "network not ready");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // wait for incoming sensor data
        size_t data_size = 0;
        sensor_data_t* sensor_data = (sensor_data_t*)xRingbufferReceive(buf_handle,
                &data_size, pdMS_TO_TICKS(30000));
        if (sensor_data == NULL) {
            ESP_LOGE(TAG, "data receive timeout");
            continue;
        } else {
            // free the ringbuffer
            vRingbufferReturnItem(buf_handle, (void*)sensor_data);
        }
        ESP_LOGI(TAG, "received sensor data (%d) from sensor (%x)", sensor_data->data, sensor_data->id);

        // generate short-name from sensor_data->id
        char short_name[16];
        snprintf(short_name, 10, "sensor_%02x", sensor_data->id);
        ESP_LOGI(TAG, "deriving short name: %s", short_name);

        // load id-context by short-name
        if (ubirch_id_context_load(short_name) == ESP_OK) {
            ESP_LOGI(TAG, "context \"%s\" could be loaded", short_name);
        } else {
            ESP_LOGI(TAG, "context \"%s\" not found, generate it", short_name);

            // check if we have a valid token
            if (!ubirch_token_state_get(UBIRCH_TOKEN_STATE_VALID)) {
                ESP_LOGW(TAG, "token not valid");
                // we cannot decide here if the token was used successfully before
                continue;
            }

            // check that we have current time before trying to generate/register keys
            time_t now = 0;
            struct tm timeinfo = {0};
            time(&now);
            localtime_r(&now, &timeinfo);
            // update time
            if (timeinfo.tm_year < (2017 - 1900)) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            // add new context
            if (ubirch_id_context_add(short_name) != ESP_OK) {
                ESP_LOGE(TAG, "failed to add context \"%s\"", short_name);
                continue;
            }

            // create uuid from gateway uuid and sensor id
            uuid_t sensor_uuid;
            if (uuid_v5_create_from_name(&sensor_uuid,
                        (char*)gateway_uuid, sizeof(gateway_uuid),
                        (char*)&sensor_data->id, sizeof(sensor_data->id)) != ESP_OK) {
            }
            if (ubirch_uuid_set(sensor_uuid, sizeof(sensor_uuid)) != ESP_OK) {
                ESP_LOGE(TAG, "failed to set uuid");
                continue;
            };
            char sensor_uuid_string[37];
            uuid_to_string(sensor_uuid, sensor_uuid_string, sizeof(sensor_uuid_string));
            ESP_LOGI(TAG, "derived uuid: %s", sensor_uuid_string);

            // create new key pair in current context
            create_keys();

            // set initial value for previous signature
            unsigned char prev_sig[64] = { 0 };
            if (ubirch_previous_signature_set(prev_sig, sizeof(prev_sig)) != ESP_OK) {
                ESP_LOGE(TAG, "failed to initialize previous signature");
                continue;
            };

            // store current context
            if (ubirch_id_context_store() != ESP_OK) {
                // probably not enough space on gateway
                ESP_LOGE(TAG, "Failed to store basic ID context");
                if (ubirch_id_context_delete(NULL) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to remove basic ID context");
                }
                // we cannot decide here if the token was used successfully before
                continue;
            }
            // --> here a new key is stored, but not yet registered
        }

        // check if id is registered
        if (!ubirch_id_state_get(UBIRCH_ID_STATE_ID_REGISTERED)) {
            // check if token is valid
            if (!ubirch_token_state_get(UBIRCH_TOKEN_STATE_VALID)) {
                // we cannot decide here if the token was used successfully before
                continue;
            }
            // call id registering function with token
            // TODO: uff! we need to distinguish the return codes in ubirch_register_current_id!
            char description[15 + 12 + 37];
            // chose an arbitrary description
            sprintf(description, "%s on gateway %s", short_name, gateway_uuid_string);
            switch (ubirch_register_current_id(description)) {
                case UBIRCH_ESP32_REGISTER_THING_SUCCESS:
                    ESP_LOGI(TAG, "id creation successfull");
                    break;
                case UBIRCH_ESP32_REGISTER_THING_ALREADY_REGISTERED:
                    ESP_LOGE(TAG, "id was already created");
                    break;
                case UBIRCH_ESP32_REGISTER_THING_ERROR:
                    ESP_LOGE(TAG, "id registration failed");
                    if (ubirch_id_context_delete(NULL) != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to remove basic ID context");
                    }
                    // we cannot decide here if the token was used successfully before
                    continue;
                    break;
            }

            ubirch_id_state_set(UBIRCH_ID_STATE_ID_REGISTERED, true);
            if (ubirch_id_context_store() != ESP_OK) {
                ESP_LOGE(TAG, "Failed to store ID context after id registration");
                continue;
            }
        }

        // check if device from current context is registered
        if (!ubirch_id_state_get(UBIRCH_ID_STATE_KEYS_REGISTERED)) {
            // check if the existing token is valid
            if (register_keys() != ESP_OK) {
                ESP_LOGW(TAG, "failed to register keys, try later");
                continue;
            }
            if (ubirch_id_context_store() != ESP_OK) {
                ESP_LOGE(TAG, "Failed to store ID context after key registration");
                continue;
            }
        }

        // get next key update timestamp
        time_t next_key_update;
        if (ubirch_next_key_update_get(&next_key_update) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read next key update");
        }

        // check if update necessary
        time_t now = time(NULL);
        if (next_key_update < now) {
            ESP_LOGI(TAG, "Your key is about to expire. Trigger key update");
            if (update_keys() != ESP_OK) {
                ESP_LOGE(TAG, "Failed to update keys");
            }
            if (ubirch_id_context_store() != ESP_OK) {
                ESP_LOGE(TAG, "Failed to store ID context after key update");
            }
        }

        // note: if we end up here we have a valid context that we can use
        // create UPP, sign it and send it to the ubirch backend
        ESP_LOGI(TAG, "create, sign and send UPP to backend");
        if (ubirch_anchor_data(&sensor_data->data, 1) != ESP_OK) {
            ESP_LOGE(TAG, "failed to anchor at ubirch backend");
        }
    }
}

/*!
 * Update the system time every 6 hours.
 *
 * @param pvParameters are currently not used, but part of the task declaration.
 */
static void update_time_task(void __unused *pvParameter) {
    EventBits_t event_bits;
    for (;;) {
        event_bits = xEventGroupWaitBits(network_event_group, (NETWORK_ETH_READY | WIFI_CONNECTED_BIT),
                                         false, false, portMAX_DELAY);
        if (event_bits & (NETWORK_ETH_READY | WIFI_CONNECTED_BIT)) {
            sntp_update();
        }
        vTaskDelay(21600000);  // delay this task for the next 6 hours
    }
}

#pragma GCC diagnostic pop

/**
 * Initialize the basic system components
 * @return ESP_OK or error code.
 */
static esp_err_t init_system() {

    esp_err_t err;
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // OTA app partition table has a smaller NVS partition size than the non-OTA
        // partition table. This size mismatch may cause NVS initialization to fail.
        // If this happens, we erase NVS partition and initialize NVS again.
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    init_nvs();

#if CONFIG_STORE_HISTORY
    initialize_filesystem();
#endif

    init_console();
    init_wifi();

    //sensor_setup();

    return err;
}


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
/*!
 * Application main function.
 * This function represents the main() functionality of other systems.
 */
void app_main() {

    //esp_err_t err;

    // initialize the system
    init_system();

    // load wifi credentials with Kconfig
    ESP_LOGI(TAG, "connecting to wifi");
    struct Wifi_login wifi;
    wifi.ssid = CONFIG_UBIRCH_WIFI_SSID;
    wifi.ssid_length = strlen(wifi.ssid);
    wifi.pwd = CONFIG_UBIRCH_WIFI_PWD;
    wifi.pwd_length = strlen(wifi.pwd);

    ESP_LOGD(TAG, "SSID: %.*s", wifi.ssid_length, wifi.ssid);
    if (wifi_join(wifi, 5000) == ESP_OK) {
        ESP_LOGI(TAG, "established");
    } else { // no connection possible
        ESP_LOGW(TAG, "no valid Wifi");
    }

    // create ringbuffer for sensor_simulator_task - main_task communication
    RingbufHandle_t buf_handle;
    buf_handle = xRingbufferCreate(1028, RINGBUF_TYPE_NOSPLIT);
    if (buf_handle == NULL) {
        printf("Failed to create ring buffer\n");
        ESP_LOGE(TAG, "failed to create ring buffer");
    }

    // create the system tasks to be executed
    xTaskCreate(&update_time_task, "sntp", 4096, NULL, 4, &net_config_handle);
    xTaskCreate(&ubirch_ota_task, "fw_update", 4096, NULL, 5, &fw_update_task_handle);
    xTaskCreate(&main_task, "main", 8192, &buf_handle, 6, &main_task_handle);
    xTaskCreate(&sensor_simulator_task, "sensor_sim", 2048, &buf_handle, 6,
            &sensor_simulator_task_handle);

    ESP_LOGI(TAG, "all tasks created");

    while (1) vTaskSuspend(NULL);
}

#pragma GCC diagnostic pop
