/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * Copyright (C) 2026 Samuel Price <thesamprice@gmail.com>
 */

/*
 * The port's event posting entry point, which src/rtems_esp_event.c
 * implements and the OS adapter's _event_post table entry calls.
 *
 * It is here rather than declared inside the adapter so the two cannot drift:
 * the signature has to match wifi_osi_funcs_t's _event_post exactly, and a
 * local extern declaration in the adapter would compile happily against a
 * changed definition.
 */

#ifndef RTEMS_ESP_EVENT_H
#define RTEMS_ESP_EVENT_H

#include <stddef.h>
#include <stdint.h>

int32_t rtems_esp_event_post(
  const char *event_base,
  int32_t     event_id,
  void       *event_data,
  size_t      event_data_size,
  uint32_t    ticks_to_wait
);

#endif /* RTEMS_ESP_EVENT_H */
