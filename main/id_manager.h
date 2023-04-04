/*!
 * @file id_manager.h
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

#ifndef EXAMPLE_ESP32_IDENTITY_MANAGER_H
#define EXAMPLE_ESP32_IDENTITY_MANAGER_H

/*!
 * @brief manage identity context, given by the \p id
 *
 * @param[in] id pointer to the identity to manage
 * @return ESP_OK if it works, ESP_FAIL if error occurs
 */
esp_err_t ubirch_id_context_manage(char *id);


#endif /* EXAMPLE_ESP32_IDENTITY_MANAGER_H */