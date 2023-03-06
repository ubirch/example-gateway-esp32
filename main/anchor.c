/*!
 * @file
 * @brief sensor handling, for user code
 *
 * ...
 *
 * @author Matthias L. Jugel
 * @date   2018-12-05
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

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <msgpack.h>
#include <message.h>
#include <ubirch_api.h>
#include <response.h>
#include <esp_log.h>
#include "driver/gpio.h"
#include <ubirch_protocol.h>
#include <ubirch_ed25519.h>
#include "anchor.h"
#include "key_handling.h"

//#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#define BLUE_LED GPIO_NUM_2
#define BOOT_BUTTON GPIO_NUM_0

extern unsigned char UUID[16];
extern unsigned char server_pub_key[crypto_sign_PUBLICKEYBYTES];

unsigned int interval = CONFIG_UBIRCH_DEFAULT_INTERVAL;

/*!
 * This function handles responses from the backend, where we can set parameters.
 * @param entry a msgpack entry as received
 */
void response_handler(const struct msgpack_object_kv *entry) {
    if (match(entry, "i", MSGPACK_OBJECT_POSITIVE_INTEGER)) {
        interval = (unsigned int) (entry->val.via.u64);
    } else {
        ESP_LOGW(__func__, "unknown configuration received: %.*s", entry->key.via.str.size, entry->key.via.str.ptr);
    }
}

/*!
 * This function handles a binary responses from the backend.
 * @param data void pointer to the received binary payload
 * @param len length of received data
 */
void bin_response_handler(const void* data, size_t len) {
    ESP_LOG_BUFFER_HEXDUMP("response UPP payload", data, len, ESP_LOG_DEBUG);
}

/*!
 * Verifier function of type ubirch_protocol_check.
 * Uses ed25519_verify_key and server_pub_key to verify signature.
 */
static int ed25519_verify_backend_response(const unsigned char *data,
        size_t len, const unsigned char signature[UBIRCH_PROTOCOL_SIGN_SIZE]) {
    return ed25519_verify_key(data, len, signature, server_pub_key);
}

esp_err_t ubirch_anchor_data(int32_t* values, uint16_t num) {
    // create a ubirch protocol context
    ubirch_protocol *upp = ubirch_protocol_new(UUID, ed25519_sign); //!< send buffer
    msgpack_unpacker *unpacker = msgpack_unpacker_new(128); //!< receive unpacker

    ubirch_message(upp, values, num);
    ESP_LOGI("UBIRCH SEND", " to %s , len: %d",CONFIG_UBIRCH_BACKEND_DATA_URL, upp->size);
    ESP_LOG_BUFFER_HEXDUMP("UPP", upp->data,upp->size, ESP_LOG_DEBUG);
    int http_status;
    switch (ubirch_send(CONFIG_UBIRCH_BACKEND_DATA_URL, UUID, upp->data, upp->size,
            &http_status, unpacker, ed25519_verify_backend_response))
    {
        case UBIRCH_SEND_OK:
            switch (http_status) {
                case 200:
                    ESP_LOGI("UBIRCH SEND", " http status of response: %d", http_status);
                    if (ubirch_parse_backend_response(unpacker, bin_response_handler)
                            != UBIRCH_ESP32_API_HTTP_RESPONSE_SUCCESS) {
                        ESP_LOGW("UBIRCH SEND", " verified response broken");
                    }
                    break;
                case 400:
                case 401:
                case 403:
                case 404:
                case 405:
                case 409:
                case 500:
                    ESP_LOGW("UBIRCH SEND", " http status of response: %d", http_status);
                    break;
                default:
                    ESP_LOGW("UBIRCH SEND", " enexpected http status: %d", http_status);
                    break;
            }
            // as the response was verified we parse it
            break;
        case UBIRCH_SEND_VERIFICATION_FAILED:
            ESP_LOGW("UBIRCH SEND", " response signature not verifiable, http status of response: %d", http_status);
            break;
        default:
            ESP_LOGE("UBIRCH_SEND", " ubirch_send failed");
            break;
    }
    ubirch_protocol_free(upp);
    msgpack_unpacker_free(unpacker);

    return ESP_OK;
}
