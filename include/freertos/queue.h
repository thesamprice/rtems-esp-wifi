/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * Copyright (C) 2026 Samuel Price <thesamprice@gmail.com>
 */

/*
 * A header shim, like the rest of include/freertos: ESP-IDF's public headers
 * name QueueHandle_t in prototypes and will not preprocess without it.
 *
 * There is no queue implementation here and there does not need to be.  Where
 * the WiFi libraries want a queue they ask for one through wifi_osi_funcs_t,
 * which is implemented on RTEMS message queues in
 * src/rtems_wifi_os_adapter.c.  A handle that crosses this boundary is
 * whatever that returned, so void * is the accurate type rather than a
 * convenient one.
 */

#ifndef RTEMS_ESP_FREERTOS_QUEUE_H
#define RTEMS_ESP_FREERTOS_QUEUE_H

#include <freertos/FreeRTOS.h>

typedef void *QueueHandle_t;

#endif /* RTEMS_ESP_FREERTOS_QUEUE_H */
