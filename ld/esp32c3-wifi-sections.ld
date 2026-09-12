/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * Placement for the section names the WiFi blobs carry and the BSP's linkcmds
 * does not know.
 *
 * Spliced into a copy of the BSP's linkcmds.base by
 * examples/wifi-init/build-and-run.sh, just above its .unexpected_sections
 * catch-all.  It is a file in this repository and not a patch against RTEMS
 * because nothing here is about the board: it is about what Espressif called
 * its sections.
 *
 * Not ld's INSERT BEFORE, which would be the obvious tool and does not work
 * here.  INSERT augments the *default* script, so it cannot refer to a section
 * defined in the same file ("`.unexpected_sections' not found for insert"),
 * and passing this as a second -T alongside the BSP's linkcmds gets it read
 * first, before the MEMORY block exists.  Passing it as -dT is worse again:
 * ld ignores a default script whenever any -T is given, so it silently does
 * nothing at all and the only symptom is the overflow it was meant to fix.
 *
 * Before this, 1549 input sections totalling 89 KiB fell through to
 * linkcmds.base's catch-all .unexpected_sections, whose region has length 0,
 * so the link failed with "region `UNEXPECTED_SECTIONS' overflowed".  That
 * catch-all is doing its job: an unplaced section is a bug, and the BSP makes
 * it an error rather than letting it land somewhere.
 *
 * ld matches input sections to the first output section that claims them, in
 * declaration order, so being above the catch-all is what matters.
 */


  /*
   * Code that must not execute from flash.
   *
   * These exist because the code runs while the flash cache is unavailable --
   * during a flash write, and in interrupt handlers -- so it has to be
   * resident in internal SRAM.  RAM_CODE is the instruction-bus window the
   * BSP carves out with ESP32C_IRAM_REGION_SIZE; 44 KiB of these sections
   * means that option has to be at least 0xB000.
   *
   * Loaded from flash and copied at startup, hence AT> and the load symbols.
   * bsp_start_copy_sections() only knows the BSP's own .fast_text, so
   * rtems_esp_wifi_copy_sections() in src/rtems_esp_wifi_init.c does this one;
   * it cannot simply be added to .fast_text because two output sections
   * cannot share a name.
   */
  .wifi_iram : ALIGN(4) {
    rtems_esp_wifi_iram_begin = .;
    *(.wifi0iram .wifi0iram.*)
    *(.wifirxiram .wifirxiram.*)
    *(.wifiextrairam .wifiextrairam.*)
    *(.wifislpiram .wifislpiram.*)
    *(.wifislprxiram .wifislprxiram.*)
    *(.wifiorslpiram .wifiorslpiram.*)
    *(.iram1 .iram1.*)
    . = ALIGN(4);
    rtems_esp_wifi_iram_end = .;
  } > RAM_CODE AT > CODE_FLASH_RAW

  rtems_esp_wifi_iram_load_begin = LOADADDR (.wifi_iram);

  /*
   * The libraries' log strings, 45 KiB of them.  Read-only and never read with
   * the cache off, so they belong in flash like any other rodata; they have
   * their own section names only so Espressif can strip them by log level.
   */
  .wifi_rodata : ALIGN(4) {
    *(.rodata_wlog_error .rodata_wlog_error.*)
    *(.rodata_wlog_warning .rodata_wlog_warning.*)
    *(.rodata_wlog_info .rodata_wlog_info.*)
    *(.rodata_wlog_debug .rodata_wlog_debug.*)
    *(.rodata_wlog_verbose .rodata_wlog_verbose.*)
  } > REGION_RODATA AT > REGION_RODATA_LOAD

  /*
   * Initialised data the libraries want in DRAM.  Half a kilobyte.  It is not
   * inside the BSP's .data, so the BSP's copy does not cover it and
   * rtems_esp_wifi_copy_sections() handles it too.
   */
  .wifi_dram : ALIGN(4) {
    rtems_esp_wifi_dram_begin = .;
    *(.dram1 .dram1.*)
    . = ALIGN(4);
    rtems_esp_wifi_dram_end = .;
  } > REGION_DATA AT > REGION_DATA_LOAD

  rtems_esp_wifi_dram_load_begin = LOADADDR (.wifi_dram);
