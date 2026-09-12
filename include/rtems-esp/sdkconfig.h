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
 * sdkconfig.h for building ESP-IDF components outside ESP-IDF.
 *
 * Every source in esp-hal-3rdparty includes this.  In ESP-IDF it is generated
 * from Kconfig by the build system; there is no Kconfig here, so it is written
 * by hand and grows one entry at a time as a source is added and asks for
 * something.
 *
 * Written rather than generated on purpose.  A generated file would carry
 * several thousand CONFIG_ symbols, and the ones that matter would be
 * indistinguishable from the ones that do not.  Here every entry is present
 * because a specific source needed it, so the file doubles as the record of
 * what has actually been built.
 *
 * Anything absent is absent because nothing has asked yet.  A source that
 * needs a new one fails at compile time with the name in the error, which is
 * the behaviour wanted: a silently-zero CONFIG_ is how a feature gets built
 * out without anyone deciding to.
 */

#ifndef RTEMS_ESP_SDKCONFIG_H
#define RTEMS_ESP_SDKCONFIG_H

/* The target.  Selects the register layouts and the per-chip branches. */
#define CONFIG_IDF_TARGET_ESP32C3 1

/*
 * Fine timing measurement.
 *
 * On for a reason that is not a preference: the blobs reference the 24
 * est_PHY_*_FTM_COMP_* variables unconditionally, and with FTM off
 * ftm_load_calibration.c defines none of them and the link fails.  So the
 * choice is FTM on, or 24 hand-written stubs whose values would be wrong.
 */
#define CONFIG_ESP_WIFI_FTM_ENABLE 1

/* The C3 is 2.4 GHz only. */
#define CONFIG_SOC_WIFI_SUPPORT_5G 0

/*
 * Maximum transmit power, in dBm, asked for by esp_phy/esp32c3/phy_init_data.c
 * -- which clamps it to 0x50 in quarter-dBm, so 20 dBm is also the ceiling the
 * data allows.  20 is ESP-IDF's own Kconfig default.
 *
 * This is a regulatory limit as well as a hardware one, and the number that
 * applies depends on where the radio is operated and on the antenna.  It is
 * here because the PHY data will not compile without it, not because 20 has
 * been checked against any particular jurisdiction; the country setting that
 * governs channels goes in separately through esp_wifi_set_country_code().
 */
#define CONFIG_ESP_PHY_MAX_TX_POWER 20

/*
 * Log level, asked for by log/include/esp_log_level.h once anything includes
 * esp_log.h -- which the supplicant's headers do.
 *
 * 3 is ESP_LOG_INFO, ESP-IDF's own default.  It only sets the compile-time
 * ceiling for ESP_LOGx in ESP-IDF sources; this port routes those through
 * printk in src/rtems_esp_glue.c rather than through esp_log's own writer, so
 * raising it produces more output and not a different mechanism.
 */
#define CONFIG_LOG_DEFAULT_LEVEL 3

/*
 * Buffer counts, all named by WIFI_INIT_CONFIG_DEFAULT() in esp_wifi.h.
 *
 * These are the branch's own Kconfig defaults, read out of
 * components/esp_wifi/Kconfig rather than remembered.  They are the largest
 * single claim the WiFi libraries make on the heap, so they are the first
 * numbers to lower if RAM gets tight.
 */
#define CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM 10
#define CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM 32
#define CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM 32

/*
 * Dynamic TX buffers, which is the Kconfig choice's own default and also what
 * it recommends: "If PSRAM is disabled, Dynamic should be selected to improve
 * the utilization of RAM."  The C3 has no PSRAM -- SOC_SPIRAM_SUPPORTED is not
 * defined for it -- so the condition is unambiguous here.
 *
 * ESP_WIFI_DYNAMIC_TX_BUFFER is the choice symbol; TX_BUFFER_TYPE is the int
 * esp_wifi.h reads, and Kconfig derives one from the other.  Both are needed
 * because esp_wifi.h tests each.
 */
#define CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER 1
#define CONFIG_ESP_WIFI_TX_BUFFER_TYPE 1

/* Static management-frame buffers, Kconfig's default. */
#define CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUF 0
#define CONFIG_ESP_WIFI_RX_MGMT_BUF_NUM_DEF 5

/* ESP-NOW peers. Two is Kconfig's default; nothing here uses ESP-NOW. */
#define CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM 2

#endif /* RTEMS_ESP_SDKCONFIG_H */
