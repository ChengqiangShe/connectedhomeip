/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <FreeRTOS.h>
#include "semphr.h"

namespace PigweedLogger {

/* Initialise an mutex variable to avoid the conflict for ESP_LOG* on UART0 */
void init();

/*Wraps ESP_LOG* into HDLC frame format*/
int putString(const char * buffer, size_t size);

SemaphoreHandle_t * GetSemaphore();

} // namespace PigweedLogger
