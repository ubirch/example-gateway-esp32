/*!
 * @file id_manager.c
 * @brief ubirch Identity manger, which takes care of the used 
 * identities.
 * 
 * This includes: 
 *  - loading and storing identity contexts
 *  - registering new identities at ubirch backend
 *  - registering keys for identities at ubirch backend
 *
 * @author Waldemar Grünwald
 * @date   2023-03-22
 *
 * @copyright &copy; 2023 ubirch GmbH (https://ubirch.com)
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

#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include <esp_err.h>

#include "keys.h"
#include "id_handling.h"
#include "key_handling.h"
#include "token_handling.h"
#include "api-http-helper.h"
#include "register_thing.h"

#include "id_manager.h"

static const char *TAG = "id_manager";

// >>> HERE THE NAMESPACE IS SET, which is later used for UUID generation
static const char *NAMESPACE = "example_namespace";
// <<<

esp_err_t ubirch_id_context_manage(char *id){
    
    char gateway_uuid_string[37];

    // generate short-name from sensor_data->id
    char short_name[16];
    snprintf(short_name, 15, "%s", id);
    ESP_LOGI(TAG, "deriving short name: %s", short_name);
        
    // load id-context by short-name
    if (ubirch_id_context_load(short_name) == ESP_OK) {
        ESP_LOGI(TAG, "context \"%s\" could be loaded", short_name);
    } 
    else {
        ESP_LOGI(TAG, "context \"%s\" not found, generate it", short_name);

        // check if we have a valid token
        if (!ubirch_token_state_get(UBIRCH_TOKEN_STATE_VALID)) {
            ESP_LOGW(TAG, "token not valid");
            // we cannot decide here if the token was used successfully before
            return ESP_FAIL; 
        }

        // check that we have current time before trying to generate/register keys
        time_t now = 0;
        struct tm timeinfo = {0};
        time(&now);
        localtime_r(&now, &timeinfo);
        // update time
        if (timeinfo.tm_year < (2017 - 1900)) {
            return ESP_FAIL; 
        }

        // add new context
        if (ubirch_id_context_add(short_name) != ESP_OK) {
            ESP_LOGE(TAG, "failed to add context \"%s\"", short_name);
            return ESP_FAIL; 
        }

        // >>> HERE THE GATEWAY ID IS SET, this part can be adapted to the needs of the user
        // load gateway-uuid
        uuid_t gateway_uuid;
        esp_efuse_mac_get_default(gateway_uuid);
        esp_base_mac_addr_set(gateway_uuid);

        // gateway uuid string, will be later used for the name in UBIRCH console
        uuid_to_string(gateway_uuid, gateway_uuid_string, sizeof(gateway_uuid_string));
        ESP_LOGI(TAG, "gateway uuid: %s", gateway_uuid_string);
        // <<<

        // derive sensor UUID
        uuid_t sensor_uuid;
        if (uuid_v5_create_derived_from_name(&sensor_uuid, 
                    (char*)NAMESPACE, sizeof(NAMESPACE),
                    (char*)gateway_uuid, sizeof(gateway_uuid),
                    short_name, sizeof(short_name)
                    ) != ESP_OK) {
            ESP_LOGE(TAG, "failed to generate uuid");
            return ESP_FAIL; 
        }
        if (ubirch_uuid_set(sensor_uuid, sizeof(sensor_uuid)) != ESP_OK) {
            ESP_LOGE(TAG, "failed to set uuid");
            return ESP_FAIL; 
        }
        char sensor_uuid_string[37];
        uuid_to_string(sensor_uuid, sensor_uuid_string, sizeof(sensor_uuid_string));
        ESP_LOGI(TAG, "derived uuid: %s", sensor_uuid_string);

        // create new key pair in current context
        create_keys();

        // set initial value for previous signature
        unsigned char prev_sig[64] = { 0 };
        if (ubirch_previous_signature_set(prev_sig, sizeof(prev_sig)) != ESP_OK) {
            ESP_LOGE(TAG, "failed to initialize previous signature");
            return ESP_FAIL; 
        }

        // store current context
        if (ubirch_id_context_store() != ESP_OK) {
            // probably not enough space on gateway
            ESP_LOGE(TAG, "Failed to store basic ID context");
            if (ubirch_id_context_delete(NULL) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to remove basic ID context");
            }
            // we cannot decide here if the token was used successfully before
            return ESP_FAIL; 
        }
        // --> here a new key is stored, but not yet registered
    }

    // check if id is registered
    if (!ubirch_id_state_get(UBIRCH_ID_STATE_ID_REGISTERED)) {
        // check if token is valid
        if (!ubirch_token_state_get(UBIRCH_TOKEN_STATE_VALID)) {
            // we cannot decide here if the token was used successfully before
            return ESP_FAIL; 
        }
        // call id registering function with token
        // >>> HERE THE DESCRIPTION FOR THE SENSOR IN THE UBIRCH CONSOLE IS CREATED
        char description[15 + 12 + 37];
        // chose an arbitrary description
        sprintf(description, "%s on gateway %s", short_name, gateway_uuid_string);
        // <<<
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
            default:
                // we cannot decide here if the token was used successfully before
                return ESP_FAIL; 
        }

        ubirch_id_state_set(UBIRCH_ID_STATE_ID_REGISTERED, true);
        if (ubirch_id_context_store() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to store ID context after id registration");
            return ESP_FAIL; 
        }
    }

    // check if device from current context is registered
    if (!ubirch_id_state_get(UBIRCH_ID_STATE_KEYS_REGISTERED)) {
        // check if the existing token is valid
        if (register_keys() != ESP_OK) {
            ESP_LOGW(TAG, "failed to register keys, try later");
            return ESP_FAIL; 
        }
        if (ubirch_id_context_store() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to store ID context after key registration");
            return ESP_FAIL; 
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

    return ESP_OK;
}


