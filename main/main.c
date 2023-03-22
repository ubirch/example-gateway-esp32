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
#include <nvs_flash.h>
#include <ubirch_ota_task.h>
#include <ubirch_ota.h>
#include <time.h>

#include "storage.h"
#include "key_handling.h"
#include "token_handling.h"
#include "anchor.h"
#include "id_manager.h"

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
    char id[16];
    int32_t data;
} sensor_data_t;

static int32_t dummy_data = 0;

/*!
 * Sensor simulator task simulates incoming sensor data.
 */
static void sensor_simulator_task(void *pvParameters) {
    RingbufHandle_t buf_handle = *(RingbufHandle_t*)pvParameters;

    // TODO: at startup create a set of sensor-id's, or use a fix set
    char sensors[2][15] = {"test_alpha", "test_beta"};
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

        ESP_LOGI(TAG, "Simulate sensor data from sensor %s", sensors[sensor_index]);
        sensor_data_t data = {};
        strcpy(data.id, sensors[sensor_index]);
        data.data = dummy_data++;

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
    EventBits_t event_bits;

    // load backend key
    if (load_backend_key() != ESP_OK) {
        ESP_LOGW(TAG, "unable to load backend key");
    }

    // load token
    if (ubirch_token_load() != ESP_OK) {
        ESP_LOGE(TAG, "failed to load token");
    }

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
        ESP_LOGI(TAG, "received sensor data (%d) from sensor (%s)", sensor_data->data, sensor_data->id);

        // manage the current ID context
        if (ubirch_id_context_manage(sensor_data->id) != ESP_OK) {
            continue;
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
