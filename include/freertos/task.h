/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * Copyright (C) 2026 Samuel Price <thesamprice@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED.
 */

/*
 * Enough of freertos/task.h for ESP-IDF's headers to parse.
 *
 * A handle type and nothing else.  ESP-IDF's public headers name
 * TaskHandle_t in prototypes; the functions behind it are reached through
 * wifi_osi_funcs_t, not from here.  See FreeRTOS.h for why there is no
 * implementation.
 */

#ifndef RTEMS_ESP_FREERTOS_TASK_H
#define RTEMS_ESP_FREERTOS_TASK_H

#include <freertos/FreeRTOS.h>

/*
 * An RTEMS task id fits in a void *, and every use of this in the headers is
 * opaque -- it is passed back to the table's task_delete or compared against
 * task_get_current_task.
 */
typedef void *TaskHandle_t;

#endif /* RTEMS_ESP_FREERTOS_TASK_H */
