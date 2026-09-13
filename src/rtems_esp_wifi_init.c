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
 *
 *     PHY_RF_CAL_NONE is not a way to shorten this under emulation, which is
 *     worth writing down because it looks like one.  Measured: it stops in
 *     exactly the same place, txdc_cal_v70+0xcc, because TX DC calibration
 *     runs whatever the mode says.  "None" refers to the slow full RF sweep,
 *     not to PHY bring-up.
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

#include <string.h>

/*
 * esp_supplicant_init() and g_wifi_default_wpa_crypto_funcs are NOT defined
 * here any more, and their absence is deliberate.
 *
 * They were weak stubs, so that a build without wpa_supplicant would link and
 * an open network would still associate.  That stopped being safe the moment
 * the real supplicant existed: a weak definition satisfies the reference, so
 * ld never searches the archive for the strong one.  The link succeeded,
 * reported zero undefined symbols, and produced an image that silently could
 * not do WPA2 -- with nm showing "W" instead of "T" as the only outward sign.
 * -u does not fix it either, because the weak definition is still a
 * definition by the time the archive is reached.
 *
 * So they come from libwpa.a now and from nowhere else.  A build that omits
 * the supplicant fails to link, naming the two symbols, which is a better
 * answer than a station that associates with nothing and cannot say why.
 *
 * Declared rather than included: esp_wpa.h drags in the supplicant's whole
 * header chain, and this file needs two symbols from it.
 */
extern int esp_supplicant_init( void );

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

/*
 * The blobs' own sections, placed by ld/esp32c3-wifi-sections.ld.
 *
 * .wifi_iram is 44 KiB of code that cannot execute from flash, so it is loaded
 * into flash and copied into the instruction-bus window at startup; .wifi_dram
 * is half a kilobyte of initialised data.  The BSP's
 * bsp_start_copy_sections() knows only about its own .fast_text, and two
 * output sections cannot share a name, so these are copied here.
 *
 * Done before register_chipv7_phy(), which is the first call that can reach
 * blob code.
 */
extern char rtems_esp_wifi_iram_begin[];
extern char rtems_esp_wifi_iram_end[];
extern char rtems_esp_wifi_iram_load_begin[];
extern char rtems_esp_wifi_dram_begin[];
extern char rtems_esp_wifi_dram_end[];
extern char rtems_esp_wifi_dram_load_begin[];

/*
 * The instruction window cannot be written through, so stores go to the same
 * SRAM through the data window.  The BSP defines the offset between the two;
 * see the comment on esp32c_iram_to_dram_delta in the BSP's linkcmds.
 */
extern char esp32c_iram_to_dram_delta[];

/*
 * The load addresses from the linker script are offsets from zero, because
 * CODE_FLASH_RAW and DATA_FLASH_RAW both have ORIGIN 0x0 -- they describe where
 * bytes sit in the flash image, not an address the CPU can read.  Reading one
 * directly faults with mcause 0x5, a load access fault, which is what the first
 * version of this function did.
 *
 * 0x3c000000 is the data-mapped flash base.  The BSP does exactly this in
 * bsps/riscv/esp32/start/bspstart.c's copy_from_flash_offset() for its own
 * .data and .fast_text; the constant is repeated here rather than shared
 * because the BSP does not export it.
 */
#define RTEMS_ESP_FLASH_MAPPED_BASE 0x3c000000

static void rtems_esp_wifi_copy_sections( void )
{
  size_t    iram_size = (size_t) ( rtems_esp_wifi_iram_end
                                     - rtems_esp_wifi_iram_begin );
  size_t    dram_size = (size_t) ( rtems_esp_wifi_dram_end
                                     - rtems_esp_wifi_dram_begin );
  uintptr_t delta     = (uintptr_t) esp32c_iram_to_dram_delta;

  if ( iram_size != 0 ) {
    /*
     * Written through the data window, not the instruction window.  The two
     * address the same SRAM and stores are only guaranteed through the data
     * bus; esp32c_iram_to_dram_delta comes from the BSP's linker script so
     * that the layout is stated in one place.
     */
    memcpy(
      (void *) ( (uintptr_t) rtems_esp_wifi_iram_begin + delta ),
      (const void *) ( (uintptr_t) rtems_esp_wifi_iram_load_begin
                         + RTEMS_ESP_FLASH_MAPPED_BASE ),
      iram_size
    );
  }

  if ( dram_size != 0 ) {
    memcpy(
      rtems_esp_wifi_dram_begin,
      (const void *) ( (uintptr_t) rtems_esp_wifi_dram_load_begin
                         + RTEMS_ESP_FLASH_MAPPED_BASE ),
      dram_size
    );
  }

  printk(
    "rtems-esp-wifi: copied %u bytes of IRAM and %u of DRAM\n",
    (unsigned) iram_size,
    (unsigned) dram_size
  );
}

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

  rtems_esp_wifi_copy_sections();

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
  printk( "rtems-esp-wifi: register_chipv7_phy( PHY_RF_CAL_FULL )...\n" );

  phy_result = register_chipv7_phy(
    &phy_init_data,
    &rtems_wifi_cal_data,
    PHY_RF_CAL_FULL
  );

  /*
   * ESP_CAL_DATA_CHECK_FAIL is not a failure, and treating it as one was a
   * bug here rather than a problem with the radio.
   *
   * register_chipv7_phy() returns it when the checksum over the *input*
   * calibration data does not match -- which is exactly what happens on a
   * first boot, because there is no saved calibration and this port has no
   * NVS to have saved it in.  libphy then does a full calibration anyway and
   * reports that it had to.  ESP-IDF's own esp_phy_load_cal_and_init() logs
   * it at INFO and carries on (esp_phy/src/phy_init.c, "Saving new
   * calibration data due to checksum failure").
   *
   * Anything else is a real failure.
   */
  if ( phy_result == ESP_CAL_DATA_CHECK_FAIL ) {
    printk(
      "rtems-esp-wifi: no saved calibration, so the PHY calibrated fully\n"
    );
  } else if ( phy_result != ESP_OK ) {
    printk( "rtems-esp-wifi: register_chipv7_phy failed (%d)\n", phy_result );

    return ESP_FAIL;
  }

  printk( "rtems-esp-wifi: PHY registered, esp_wifi_init_internal()...\n" );

  result = esp_wifi_init_internal( config );

  if ( result != ESP_OK ) {
    printk( "rtems-esp-wifi: esp_wifi_init_internal failed (%d)\n", result );

    return result;
  }

  printk( "rtems-esp-wifi: libraries initialised, esp_supplicant_init()...\n" );

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
