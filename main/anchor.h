/*!
 * @file
 * @brief TODO: ${FILE}
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
#ifndef EXAMPLE_ESP32_SENSOR_H
#define EXAMPLE_ESP32_SENSOR_H

/*!
 * Create UPP from array of 32-bit integers and send the UPP (including
 * a hash value of your data) to the ubirch backend.
 *
 * This is an example implementation of how to create and send requests
 * to the ubirch backend. Information about the response is logged but
 * not properly handled (the function always returns ESP_OK).
 * The implementation and the error handling should be adapted to your
 * needs and the data you want to anchor.
 *
 * @param values Array fo 32-bit integers
 * @param num Length of values array
 * @return ESP_OK
 */
esp_err_t ubirch_anchor_data(int32_t* values, uint16_t num);

#endif //EXAMPLE_ESP32_SENSOR_H
