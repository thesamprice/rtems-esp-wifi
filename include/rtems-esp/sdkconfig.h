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

#endif /* RTEMS_ESP_SDKCONFIG_H */
