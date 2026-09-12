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
 * esp_wifi_init() for RTEMS.
 *
 * This is the port's own, not ESP-IDF's.  ESP-IDF's esp_wifi/src/wifi_init.c
 * is in esp-hal-3rdparty, but five of its includes are not -- esp_pm.h,
 * esp_private/pm_impl.h, esp_psram.h, hal/adc_types.h and esp_wpa.h -- so it
 * cannot be compiled from the branch, and following them into esp-idf proper
 * is the thing docs/step5-scope.md explains we are avoiding.  NuttX writes its
 * own for the same reason.
 *
 * Read ESP-IDF's with power management, PSRAM, coexistence, MAC/BB power down,
 * NAN, the roaming app and tickless idle all off and nine calls are left.
 * Seven are in libnet80211.a.  This file is those seven in ESP-IDF's order,
 * plus the PHY registration, and it deliberately does no more than that: the
 * point of the exercise is a station that associates, not feature parity.
 *
 * What this file does NOT do, so nobody looks for it here:
 *
 *   * No NVS.  register_chipv7_phy() is called with PHY_RF_CAL_FULL, which
 *     calibrates from scratch.  ESP-IDF uses NVS only to cache the result and
 *     shorten the next boot; it is not required to bring the radio up.  The
 *     cost is calibration time on every boot.
 *   * No coexistence.  There is no Bluetooth here to coexist with.
 *   * No sleep.  Every power-management entry point in ESP-IDF's version is
 *     behind CONFIG_PM_ENABLE or CONFIG_FREERTOS_USE_TICKLESS_IDLE.
 */

/* Before the esp_private headers: phy.h names _lock_t, which RTEMS' newlib
 * does not have. */
#include <rtems-esp/newlib-compat.h>

#include <esp_wifi.h>
#include <esp_private/wifi.h>
#include <esp_phy_init.h>
#include <esp_private/phy.h>

#include <rtems.h>
#include <rtems/bspIo.h>

/*
 * The one thing still missing, and it is deliberate rather than forgotten.
 *
 * esp_supplicant_init() is what ESP-IDF's esp_wifi_init() calls to bring up
 * WPA2, and without it an association to any encrypted AP fails.  The
 * supplicant is components/wpa_supplicant in the same branch -- 145 C files
 * and mbedtls -- so it is a build to arrange, not code to write, and it is
 * kept as its own step rather than smuggled in here.
 *
 * Declared weak so that linking the supplicant in is the only change needed:
 * with it present the real one wins, and without it this reports rather than
 * failing to link.  An open network would associate either way, which is what
 * makes this worth having now instead of after the supplicant.
 */
int __attribute__((weak)) esp_supplicant_init( void )
{
  printk(
    "rtems-esp-wifi: esp_supplicant_init is not linked in; "
    "only open networks will associate\n"
  );

  return ESP_OK;
}

/*
 * ESP-IDF's sleep defaults, in the units its own esp_wifi_init() converts to.
 * They are set even with sleep disabled because the values live in the WiFi
 * library's state rather than in a sleep driver, and leaving them at zero is
 * not the same as leaving them alone.
 */
#define RTEMS_WIFI_MIN_ACTIVE_TIME_US            ( 50u * 1000u )
#define RTEMS_WIFI_KEEP_ALIVE_TIME_US            ( 10u * 1000u * 1000u )
#define RTEMS_WIFI_WAIT_BROADCAST_DATA_TIME_US   ( 15u * 1000u )

/* From components/esp_phy/esp32c3/phy_init_data.c, which the branch ships as
 * source.  It is the per-chip PHY register set; there is no way to derive it
 * and no default. */
extern const esp_phy_init_data_t phy_init_data;

static bool rtems_wifi_inited;

/*
 * Calibration data is an output here, not an input, because the mode is
 * PHY_RF_CAL_FULL.  It is kept rather than discarded so that a later step can
 * write it somewhere and switch to PHY_RF_CAL_NONE.
 */
static esp_phy_calibration_data_t rtems_wifi_cal_data;

esp_err_t esp_wifi_init( const wifi_init_config_t *config )
{
  esp_err_t result;
  int phy_result;

  if ( config == NULL ) {
    return ESP_ERR_INVALID_ARG;
  }

  if ( rtems_wifi_inited ) {
    printk( "rtems-esp-wifi: already initialised\n" );

    return ESP_ERR_INVALID_STATE;
  }

  /*
   * The OS adapter has to be reachable before anything in the libraries runs.
   * It travels in the config -- WIFI_INIT_CONFIG_DEFAULT() sets
   * .osi_funcs = &g_wifi_osi_funcs -- and esp_wifi_init_internal() is what
   * assigns the ROM's g_osi_funcs_p from it.  A caller that builds a config by
   * hand and leaves this null gets a null-pointer call from inside a blob, so
   * it is worth refusing here where the message can say why.
   */
  if ( config->osi_funcs == NULL ) {
    printk( "rtems-esp-wifi: config->osi_funcs is null; "
            "build the config with WIFI_INIT_CONFIG_DEFAULT()\n" );

    return ESP_ERR_INVALID_ARG;
  }

  esp_wifi_set_sleep_min_active_time( RTEMS_WIFI_MIN_ACTIVE_TIME_US );
  esp_wifi_set_keep_alive_time( RTEMS_WIFI_KEEP_ALIVE_TIME_US );
  esp_wifi_set_sleep_wait_broadcast_data_time(
    RTEMS_WIFI_WAIT_BROADCAST_DATA_TIME_US
  );

  /*
   * Full calibration, so this is the slow path by design -- see the file
   * comment.  register_chipv7_phy() returns ESP_CAL_DATA_CHECK_FAIL when it
   * rejects *input* calibration data, which cannot happen here, so anything
   * non-zero is worth reporting rather than ignoring.
   */
  phy_result = register_chipv7_phy(
    &phy_init_data,
    &rtems_wifi_cal_data,
    PHY_RF_CAL_FULL
  );

  if ( phy_result != ESP_OK ) {
    printk( "rtems-esp-wifi: register_chipv7_phy failed (%d)\n", phy_result );

    return ESP_FAIL;
  }

  result = esp_wifi_init_internal( config );

  if ( result != ESP_OK ) {
    printk( "rtems-esp-wifi: esp_wifi_init_internal failed (%d)\n", result );

    return result;
  }

  result = esp_supplicant_init();

  if ( result != ESP_OK ) {
    printk( "rtems-esp-wifi: esp_supplicant_init failed (%d)\n", result );

    /*
     * Undo the internal init rather than leaving the libraries half up, which
     * is what ESP-IDF does on this path.  A second esp_wifi_init() after a
     * failure should be able to start from the same place as the first.
     */
    (void) esp_wifi_deinit_internal();

    return result;
  }

  rtems_wifi_inited = true;

  return ESP_OK;
}

/*
 * ESP-IDF's are one line each as well; the rest of their bodies is the roaming
 * app, which is behind CONFIG_ESP_WIFI_ENABLE_ROAMING_APP and is not built
 * here.  They exist because libmesh and libsmartconfig reference the
 * unsuffixed names, and those archives cannot be dropped -- libnet80211 and
 * libpp reference mesh and espnow symbols themselves.
 */
esp_err_t esp_wifi_connect( void )
{
  return esp_wifi_connect_internal();
}

esp_err_t esp_wifi_disconnect( void )
{
  return esp_wifi_disconnect_internal();
}
