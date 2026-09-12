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
 * Enough of FreeRTOS.h for ESP-IDF's headers to parse on RTEMS.
 *
 * This is a header shim, not a FreeRTOS emulation.  ESP-IDF's public headers
 * include <freertos/FreeRTOS.h> for a handful of types -- TickType_t and a
 * couple of constants -- and without them nothing downstream will preprocess.
 * That is all this provides.
 *
 * docs/architecture.md in the rtems-esphome tree rules out emulating
 * FreeRTOS, and this does not: no scheduler, no queues, no tasks.  Where the
 * WiFi libraries need those, they get them through wifi_osi_funcs_t, which is
 * a table of function pointers the port fills with RTEMS calls -- the seam
 * Espressif designed for exactly this.  Anything that needs more than a type
 * from here should be using that table instead, and a compile error naming
 * the missing thing is the right outcome rather than a stub that pretends.
 */

#ifndef RTEMS_ESP_FREERTOS_H
#define RTEMS_ESP_FREERTOS_H

#include <rtems.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* FreeRTOS counts ticks in an unsigned 32-bit word; so does RTEMS. */
typedef uint32_t TickType_t;
typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;

#define portMAX_DELAY ( (TickType_t) 0xffffffffU )

/*
 * Milliseconds per tick.  RTEMS knows this at run time rather than at
 * compile time -- CONFIGURE_MICROSECONDS_PER_TICK is the application's
 * choice, not the BSP's -- so this is a call where FreeRTOS has a constant.
 * Correct, and not a constant expression: anything using it in a static
 * initialiser will not compile, which is better than being silently wrong for
 * a configuration that is not 1 kHz.
 */
#define portTICK_PERIOD_MS \
  ( rtems_configuration_get_microseconds_per_tick() / 1000 )

#define pdMS_TO_TICKS( ms ) \
  ( (TickType_t) RTEMS_MILLISECONDS_TO_TICKS( ms ) )

#define pdTRUE  ( (BaseType_t) 1 )
#define pdFALSE ( (BaseType_t) 0 )
#define pdPASS  pdTRUE
#define pdFAIL  pdFALSE

#endif /* RTEMS_ESP_FREERTOS_H */
