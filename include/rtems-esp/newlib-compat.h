/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * Copyright (C) 2026 Samuel Price <thesamprice@gmail.com>
 */

/*
 * Names ESP-IDF's headers expect from *their* newlib that RTEMS' does not
 * spell the same way.
 *
 * Include this before any esp_private header.  It is not a shim for a whole
 * subsystem the way include/freertos is; ESP-IDF's newlib carries a few
 * extensions and their public headers name them in prototypes.
 *
 * Keep it to types only.  A function RTEMS genuinely lacks belongs in
 * src/rtems_esp_glue.c where it can be implemented, not declared away here.
 */

#ifndef RTEMS_ESP_NEWLIB_COMPAT_H
#define RTEMS_ESP_NEWLIB_COMPAT_H

/*
 * RTEMS' newlib has <sys/lock.h> and defines _LOCK_T, as
 * struct _Mutex_Control.  What it does not define is the lowercase _lock_t
 * alias, which ESP-IDF's newlib adds and which esp_private/phy.h names in
 * phy_get_lock()'s return type.
 *
 * So this is one typedef over RTEMS' real mutex type, not a stand-in for it.
 * Getting that wrong -- declaring _LOCK_T here as void * -- collides with
 * <sys/lock.h> as soon as anything pulls in <stdio.h>, which esp_err.h does.
 */
#include <sys/lock.h>

#ifndef _lock_t
typedef _LOCK_T _lock_t;
#endif

#endif /* RTEMS_ESP_NEWLIB_COMPAT_H */
