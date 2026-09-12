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

#endif /* RTEMS_ESP_SDKCONFIG_H */
