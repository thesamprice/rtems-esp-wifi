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

/*
 * vTaskDelay() is declared here, and implemented in src/rtems_esp_glue.c,
 * which is a departure from the rest of this directory.
 *
 * Everything else in include/freertos is types only, deliberately: where the
 * WiFi libraries need to block or sleep they ask through wifi_osi_funcs_t,
 * which is the seam Espressif designed for exactly that, and the file comment
 * says a compile error is preferred to a stub.
 *
 * wpa_supplicant's port/eloop.c does not use that seam.  It calls vTaskDelay()
 * directly, twice, because it was written against FreeRTOS rather than against
 * the adapter.  So the choice is not "declaration or compile error" -- the
 * compile error has no fix on the far side, since the supplicant is upstream
 * source this port does not modify.  The choice is a real implementation or no
 * supplicant.
 *
 * It is a real implementation and not a stub: the adapter's own task_delay
 * entry is rtems_task_wake_after() on the same tick units, and this is the
 * same call.  A stub returning immediately would turn eloop's wait loops into
 * busy spins on a single-core part.
 */
void vTaskDelay( TickType_t ticks );

#endif /* RTEMS_ESP_FREERTOS_TASK_H */
