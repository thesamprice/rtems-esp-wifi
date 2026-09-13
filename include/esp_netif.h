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
 * The netif has since been written and it is NOT here: it is
 * include/rtems-esp/netif.h and src/rtems_esp_netif.c, an lwIP netif of the
 * same shape as rtems-lwip's Zynq GEM driver.  This file stays the width it
 * was, which is the whole point of the exercise -- the port needs an
 * interface, not esp_netif's API.  Filling the rest of esp_netif in would mean
 * implementing esp_netif_new, _attach, _set_driver_config, the
 * esp_netif_action_* family and an IP_EVENT loop, and then implementing an
 * lwIP netif underneath all of it anyway, since that is what esp_netif is a
 * wrapper for.  ESP32 Open MAC does go that way, because they are inside
 * ESP-IDF and esp_netif is already built for them; here it would be a second
 * abstraction with one user.
 *
 * So the two typedefs below are all of it, and they are unchanged.
 */

/*
 * The guard is deliberately not RTEMS_ESP_NETIF_H, which is what
 * <rtems-esp/netif.h> uses.  It was, and the two collided: a file including
 * the real netif header first got this one silently skipped, and the failure
 * arrived as ESP-IDF's own esp_wifi_default.h not knowing what esp_netif_t is.
 * Loud, but a long way from the cause.
 */
#ifndef RTEMS_ESP_NETIF_COMPAT_H
#define RTEMS_ESP_NETIF_COMPAT_H

#include <stdint.h>

/*
 * Opaque.  ESP-IDF defines a large structure behind this; nothing on RTEMS
 * looks inside it, and leaving it incomplete means a translation unit that
 * tries to will not compile rather than silently disagreeing about its size.
 *
 * Incomplete rather than aliased to the port's netif, which was considered:
 * making esp_netif_t a struct netif would let a handle pass through these
 * prototypes and mean something.  But nothing calls them -- that is why they
 * are only here to parse -- so the alias would exist to make a fiction look
 * truthful, and would put lwIP on the include path of every ESP-IDF source
 * this port compiles.
 */
typedef struct esp_netif_obj esp_netif_t;
typedef struct esp_netif_inherent_config esp_netif_inherent_config_t;

#endif /* RTEMS_ESP_NETIF_COMPAT_H */
