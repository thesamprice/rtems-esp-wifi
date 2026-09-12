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
 * esp_netif.h -- and this one belongs to the port, not to Espressif.
 *
 * esp_netif is deliberately **not** in esp-hal-3rdparty.  The branch ships 34
 * components and the netif abstraction is not one of them, because a
 * third-party framework brings its own network stack: NuttX has none of it
 * (zero references to esp_netif in their WiFi adapter) and neither will this.
 *
 * So this header exists only so that ESP-IDF's public headers parse.
 * esp_wifi.h includes esp_wifi_default.h, which names esp_netif_t in
 * prototypes; nothing in the WiFi libraries calls those functions, and
 * nothing here defines them.  A configuration that calls one gets a link
 * error, which is the correct outcome: the answer is a netif on rtems-lwip,
 * not an esp_netif.
 *
 * When that netif is written -- step 5 in the README -- it goes here, and
 * this comment is the record of why the file was empty first.
 */

#ifndef RTEMS_ESP_NETIF_H
#define RTEMS_ESP_NETIF_H

#include <stdint.h>

/*
 * Opaque.  ESP-IDF defines a large structure behind this; nothing on RTEMS
 * looks inside it, and leaving it incomplete means a translation unit that
 * tries to will not compile rather than silently disagreeing about its size.
 */
typedef struct esp_netif_obj esp_netif_t;
typedef struct esp_netif_inherent_config esp_netif_inherent_config_t;

#endif /* RTEMS_ESP_NETIF_H */
