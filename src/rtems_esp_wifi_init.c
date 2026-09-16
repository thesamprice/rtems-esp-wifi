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
 * Keeps the USB PHY alive across WiFi PHY initialisation.
 *
 * This is a silicon workaround rather than a preference.  ESP-IDF gates it on
 * SOC_WIFI_PHY_NEEDS_USB_WORKAROUND and gives CONFIG_ESP_PHY_ENABLE_USB
 * "default y if IDF_TARGET_ESP32C3", and esp_phy_load_cal_and_init() calls it
 * immediately before register_chipv7_phy().  The WiFi PHY and the
 * USB-Serial-JTAG share the BBPLL, so bringing the one up without this takes
 * the other down with it -- and here the console *is* the USB-Serial-JTAG.
 *
 * Declared rather than included for the same reason as esp_supplicant_init()
 * above: it is private to esp_phy and the header is not installed.
 */
extern void phy_bbpll_en_usb( bool en );

/* ROM, via esp32c3.rom.ld.  Retimes ets_delay_us() for a new CPU frequency. */
extern void ets_update_cpu_frequency( uint32_t cpu_mhz );

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

/* === the modem power domain ============================================= */

/*
 * Powering the radio up, which ESP-IDF does in esp_wifi_init() and this port
 * did not.
 *
 * It was skipped because esp_wifi_power_domain_on() lives in esp_phy, which is
 * not built here, and because nothing in QEMU noticed: every register below is
 * answered by the machine's catch-all regardless of whether anything was
 * written to it, so calibration converged in emulation with the radio still
 * powered down and isolated.
 *
 * On silicon it does not.  The first hardware run hung in ram_iq_est_enable's
 * convergence loop -- reading a measurement, shifting right 12, masking seven
 * bits, and looping while it stayed below a threshold -- until the timer
 * group 0 watchdog reset the part, about once a second, forever.  The IQ
 * estimate cannot converge on analog blocks that have no power.
 *
 * The sequence is esp_phy/src/phy_init.c's esp_wifi_bt_power_domain_on(), with
 * the reference counting and the lock removed: this port brings the radio up
 * once and never takes it down, so there is nothing to count.
 */

/*
 * These duplicate the accessors and SYSCON addresses in
 * src/rtems_wifi_os_adapter.c, which has its own for the clock and reset
 * entries of the OS adapter table.  Sharing them would mean a header for two
 * inline functions and four constants, and putting the power-domain sequence
 * in the adapter would be worse -- it is not an OS primitive, it is part of
 * bringing the radio up, which is what this file is.
 */
static inline uint32_t rtems_wifi_reg_read( uint32_t addr )
{
  return *(volatile uint32_t *) (uintptr_t) addr;
}

static inline void rtems_wifi_reg_write( uint32_t addr, uint32_t value )
{
  *(volatile uint32_t *) (uintptr_t) addr = value;
}

#define RTEMS_WIFI_SYSCON_BASE        0x60026000u
#define RTEMS_WIFI_CLK_EN_REG         ( RTEMS_WIFI_SYSCON_BASE + 0x014u )
#define RTEMS_WIFI_RST_EN_REG         ( RTEMS_WIFI_SYSCON_BASE + 0x018u )

/* soc/esp32c3/register/soc/rtc_cntl_reg.h */
#define RTEMS_WIFI_RTC_BASE           0x60008000u
#define RTEMS_WIFI_RTC_DIG_PWC_REG    ( RTEMS_WIFI_RTC_BASE + 0x0088u )
#define RTEMS_WIFI_RTC_WIFI_FORCE_PD    ( 1u << 17 )
#define RTEMS_WIFI_RTC_WIFI_FORCE_PU    ( 1u << 18 )
#define RTEMS_WIFI_RTC_WIFI_PD_EN       ( 1u << 30 )
#define RTEMS_WIFI_RTC_DIG_ISO_REG      ( RTEMS_WIFI_RTC_BASE + 0x008Cu )
#define RTEMS_WIFI_RTC_WIFI_FORCE_ISO   ( 1u << 28 )
#define RTEMS_WIFI_RTC_WIFI_FORCE_NOISO ( 1u << 29 )

/* syscon_reg.h: SYSTEM_WIFI_CLK_WIFI_BT_COMMON_M, and MODEM_RESET_FIELD_WHEN_PU
 * spelled out because the header composes it from ten BT and WiFi bits. */
#define RTEMS_WIFI_CLK_BT_COMMON_M    0x78078Fu
#define RTEMS_WIFI_CLK_PHY_EN_M       0x400000u
/*
 * The WiFi MAC's clock.  No public header names it: it is in neither
 * SYSTEM_WIFI_CLK_WIFI_BT_COMMON_M nor SYSTEM_WIFI_CLK_PHY_EN_M, and
 * SYSTEM_WIFI_CLK_WIFI_EN_M -- which is what ESP-IDF's wifi_module_enable()
 * sets for PERIPH_WIFI_MODULE -- is defined as 0 on this chip, so that call is
 * a no-op here and something else has to do the work.
 *
 * Found by bisection, not by reading.  Without it the MAC window at
 * 0x60033000 answers none of its first 64 words and hal_init() spins forever
 * on the bring-up acknowledge in MAC[0x0d14]; with it the window answers 48 of
 * 64 and esp_wifi_start() returns.
 */
#define RTEMS_WIFI_CLK_MAC_EN         ( 1u << 6 )
#define RTEMS_WIFI_MODEM_RESET_WHEN_PU                                        \
  ( ( 1u << 0 ) | ( 1u << 1 ) | ( 1u << 2 ) | ( 1u << 3 ) |                    \
    ( 1u << 4 ) | ( 1u << 9 ) | ( 1u << 11 ) | ( 1u << 13 ) )

/*
 * The blocks register_chipv7_phy() calibrates against, for the probe below.
 * reg_base.h names FE and BB; 0x6001C000 is the AGC block, which that header
 * does not name but which sits immediately below DR_REG_NRX_BASE (0x6001CC00)
 * exactly as AGC does on the original ESP32.
 */
#define RTEMS_WIFI_FE_BASE            0x60006000u
#define RTEMS_WIFI_FE_IQ_EST_REG      ( RTEMS_WIFI_FE_BASE + 0x174u )
#define RTEMS_WIFI_FE_IQ_EST_DONE     ( 1u << 16 )
#define RTEMS_WIFI_AGC_BASE           0x6001C000u
#define RTEMS_WIFI_AGC_STATE_REG      ( RTEMS_WIFI_AGC_BASE + 0x08Cu )

/* soc/esp32c3/regi2c_defs.h: the internal I2C analog master. */
#define RTEMS_WIFI_I2C_MST_ANA_CONF0_REG  0x6000E040u
#define RTEMS_WIFI_ANA_CONFIG_REG         0x6000E044u
#define RTEMS_WIFI_ANA_CONFIG2_REG        0x6000E048u
#define RTEMS_WIFI_ANA_CONFIG_M           ( 0x3FFu << 8 )
#define RTEMS_WIFI_ANA_I2C_SAR_FORCE_PD   ( 1u << 18 )
#define RTEMS_WIFI_ANA_I2C_SAR_FORCE_PU   ( 1u << 16 )

/* soc/esp32c3/register/soc/system_reg.h */
#define RTEMS_WIFI_SYSTEM_CPU_PER_CONF_REG  0x600C0008u
#define RTEMS_WIFI_SYSTEM_SYSCLK_CONF_REG   0x600C0058u

/*
 * Which calibration register_chipv7_phy() is asked for.  Overridable at build
 * time because the three modes are the cheapest way to tell a calibration that
 * cannot converge from one that is never reached: PHY_RF_CAL_NONE skips the
 * estimators entirely.
 */
#ifndef RTEMS_WIFI_CAL_MODE
#define RTEMS_WIFI_CAL_MODE PHY_RF_CAL_FULL
#endif

/* soc/esp32c3/register/soc/efuse_reg.h */
#define RTEMS_WIFI_EFUSE_BASE         0x60008800u
#define RTEMS_WIFI_EFUSE_MAC0_REG     ( RTEMS_WIFI_EFUSE_BASE + 0x44u )
#define RTEMS_WIFI_EFUSE_MAC1_REG     ( RTEMS_WIFI_EFUSE_BASE + 0x48u )

/*
 * Says whether the blocks the calibration spins on are clocked.
 *
 * register_chipv7_phy() hangs inside ram_iq_est_enable() polling
 * FE[0x174] bit 16, the IQ estimator's done flag, with no timeout: the loop's
 * only exit is that bit going high.  A dead clock and a mis-programmed
 * estimator look identical from outside, so this samples the AGC state word
 * twice.  AGC[0x8c] bits [18:12] are what that same loop reads, and they are
 * the counter ram_iq_est_enable() compares against 69 -- so if they do not
 * move between two reads the block has no clock, and if they do the clock is
 * fine and the estimator itself is not converging.
 *
 * Printed rather than asserted.  The point is to distinguish two causes, not
 * to pass or fail, and the numbers are the evidence.
 */
#ifdef RTEMS_WIFI_DEBUG_NO_WDT
/*
 * Turns every watchdog off, so a hang inside the PHY blob stays a hang.
 *
 * Debugging only, and deliberately not the default.  register_chipv7_phy()
 * spins with no timeout, the TIMG0 watchdog resets the chip about 1.5s later,
 * and the reset tears down the USB device -- which takes the built-in JTAG
 * with it, so OpenOCD cannot finish examining the debug module before the next
 * reset.  Without the resets the board sits still in the spin loop and can be
 * halted and inspected.
 *
 * The super-watchdog has no disable bit; auto-feed is the documented way to
 * stop it.  Each of the four is behind a write-protect key.
 */
static void rtems_esp_wifi_watchdogs_off( void )
{
  rtems_wifi_reg_write( 0x6001F064u, 0x50D83AA1u );  /* TIMG0 WDT unlock */
  rtems_wifi_reg_write( 0x6001F048u, 0u );
  rtems_wifi_reg_write( 0x60020064u, 0x50D83AA1u );  /* TIMG1 WDT unlock */
  rtems_wifi_reg_write( 0x60020048u, 0u );
  rtems_wifi_reg_write( 0x600080A8u, 0x50D83AA1u );  /* RTC WDT unlock */
  rtems_wifi_reg_write( 0x60008090u, 0u );
  rtems_wifi_reg_write( 0x600080B0u, 0x8F1D312Au );  /* RTC SWD unlock */
  rtems_wifi_reg_write( 0x600080ACu, 0x80000000u );  /* SWD_AUTO_FEED_EN */
  rtems_wifi_reg_write( 0x600080B0u, 0u );

  printk( "rtems-esp-wifi: watchdogs disabled, a hang will now stay a hang\n" );
}
#endif

#ifdef RTEMS_WIFI_DEBUG_PROBE
/*
 * Does a register window answer the bus?
 *
 * Reading a modem register gives zero whether the block is absent or merely
 * idle, so a read cannot tell the two apart.  A write can: invert a word,
 * read it back, put the original back.  Restoring matters because the blob
 * runs afterwards and would otherwise inherit a corrupted register, and the
 * restore is exact because the saved value is what the bus itself reported.
 *
 * Any single register may be read-only or reserved and would report "dark" on
 * a live bus, so this counts over a whole window instead of trusting one
 * address.  That distinction is what identified the PHY calibration hang: FE
 * answered 52 of 64 words while AGC, NRX and BB answered none.
 */
static bool rtems_esp_wifi_reg_writable( uint32_t addr )
{
  uint32_t saved = rtems_wifi_reg_read( addr );
  uint32_t back;

  rtems_wifi_reg_write( addr, ~saved );
  back = rtems_wifi_reg_read( addr );
  rtems_wifi_reg_write( addr, saved );

  return back != saved;
}

static void rtems_esp_wifi_probe_window(
  const char *name,
  uint32_t    base,
  unsigned    words
)
{
  unsigned live = 0;
  unsigned i;

  for ( i = 0; i < words; ++i ) {
    if ( rtems_esp_wifi_reg_writable( base + i * 4u ) ) {
      ++live;
    }
  }

  printk( "rtems-esp-wifi: probe %s %08x: %u/%u writable\n",
          name, base, live, words );
}
#endif

/*
 * Puts the CPU on the BBPLL at 160MHz, so the APB runs at 80.
 *
 * ESP-IDF does this in the second-stage bootloader's rtc_clk_init(), and a
 * direct-boot image has no second stage: the ROM leaves the CPU on the 40MHz
 * crystal, SYSTEM_SYSCLK_CONF reads SOC_CLK_SEL 0, and everything else in the
 * system is happy there.  The PHY is not.  ESP-IDF holds an
 * ESP_PM_APB_FREQ_MAX lock for as long as WiFi is up precisely because the
 * radio's timing is specified against an 80MHz APB, and on the C3 the APB is
 * 80MHz exactly when the CPU is driven from the PLL.
 *
 * ets_delay_us() is retimed afterwards because it is calibrated in CPU ticks
 * and ram_iq_est_enable() calls it between arming the estimator and polling
 * for the result.
 *
 * The BBPLL itself is already running -- the USB-Serial-JTAG that carries this
 * console is fed from it -- so only the CPU mux and the divider move here.
 */
static void rtems_esp_wifi_cpu_clock_to_pll( void )
{
  uint32_t reg;

  /* CPUPERIOD_SEL = 1, which is 160MHz given PLL_FREQ_SEL's 480MHz. */
  reg = rtems_wifi_reg_read( RTEMS_WIFI_SYSTEM_CPU_PER_CONF_REG );
  reg = ( reg & ~0x3u ) | 1u;
  rtems_wifi_reg_write( RTEMS_WIFI_SYSTEM_CPU_PER_CONF_REG, reg );

  /* Then the mux, and PRE_DIV_CNT back to divide-by-one.  This order is
   * ESP-IDF's: the period is chosen before the source that makes it apply. */
  reg = rtems_wifi_reg_read( RTEMS_WIFI_SYSTEM_SYSCLK_CONF_REG );
  reg &= ~0x3FFu;
  reg = ( reg & ~( 0x3u << 10 ) ) | ( 1u << 10 );
  rtems_wifi_reg_write( RTEMS_WIFI_SYSTEM_SYSCLK_CONF_REG, reg );

  ets_update_cpu_frequency( 160u );
}

static void rtems_esp_wifi_power_domain_on( void )
{
  uint32_t reg;

  /*
   * Powered, and forced so rather than left to the automatic control.
   *
   * ESP-IDF only clears WIFI_FORCE_PD here, because it can rely on
   * WIFI_FORCE_PU and WIFI_FORCE_NOISO still holding their reset value of 1.
   * On this boot path they do not: read back on hardware, DIG_PWC is
   * 0x00000800 and DIG_ISO is 0x00400080 by the time the port runs -- BT
   * forced down and isolated, and every FORCE_PU/FORCE_NOISO bit clear,
   * including WiFi's.  With neither FORCE_PU nor FORCE_PD set the domain is
   * under automatic control, and the whole modem digital block reads back as
   * zero: FE[0x174] and AGC[0x8c] are 0, writes do not stick, and
   * register_chipv7_phy() spins forever in ram_iq_est_enable() waiting on a
   * done flag in a block that is not there.
   *
   * So set the two positives as well as clearing the two negatives, and clear
   * WIFI_PD_EN so nothing power-gates the domain behind our back.
   */
  reg = rtems_wifi_reg_read( RTEMS_WIFI_RTC_DIG_PWC_REG );
  reg &= ~( RTEMS_WIFI_RTC_WIFI_FORCE_PD | RTEMS_WIFI_RTC_WIFI_PD_EN );
  reg |= RTEMS_WIFI_RTC_WIFI_FORCE_PU;
  rtems_wifi_reg_write( RTEMS_WIFI_RTC_DIG_PWC_REG, reg );

  /*
   * ESP-IDF waits 10us here for the rails to come up before touching the
   * modem.  rtems_task_wake_after() cannot express that -- it would round to a
   * whole tick, which is 10ms and would also yield -- and this runs before the
   * radio exists, so a busy wait is both correct and cheap.  The systimer is
   * already running; one tick of it is well under a microsecond.
   */
  {
    uint64_t start = rtems_clock_get_uptime_nanoseconds();

    while ( rtems_clock_get_uptime_nanoseconds() - start < 10000u ) {
      /* wait */
    }
  }

  /* The common clock has to be on across the reset pulse. */
  reg = rtems_wifi_reg_read( RTEMS_WIFI_CLK_EN_REG );
  rtems_wifi_reg_write( RTEMS_WIFI_CLK_EN_REG,
                        reg | RTEMS_WIFI_CLK_BT_COMMON_M );

  /* Reset the modem now it has power: assert, then release. */
  reg = rtems_wifi_reg_read( RTEMS_WIFI_RST_EN_REG );
  rtems_wifi_reg_write( RTEMS_WIFI_RST_EN_REG,
                        reg | RTEMS_WIFI_MODEM_RESET_WHEN_PU );
  rtems_wifi_reg_write( RTEMS_WIFI_RST_EN_REG,
                        reg & ~RTEMS_WIFI_MODEM_RESET_WHEN_PU );

  /*
   * Out of isolation.  Order matters: power, then reset, then this.
   * FORCE_NOISO for the same reason as FORCE_PU above -- clearing FORCE_ISO
   * alone only returns the domain to automatic isolation, which is the state
   * it was already in and which keeps the block dark.
   */
  reg = rtems_wifi_reg_read( RTEMS_WIFI_RTC_DIG_ISO_REG );
  reg &= ~RTEMS_WIFI_RTC_WIFI_FORCE_ISO;
  reg |= RTEMS_WIFI_RTC_WIFI_FORCE_NOISO;
  rtems_wifi_reg_write( RTEMS_WIFI_RTC_DIG_ISO_REG, reg );

  /*
   * The common clock goes back OFF here to match ESP-IDF's power-domain
   * function, which toggles it only around the reset pulse.  It is turned on
   * again for good in rtems_esp_wifi_clocks_on() below -- keeping the two
   * separate because they are separate steps upstream and conflating them
   * hides that the PHY needs a clock the power-up sequence does not leave on.
   */
  reg = rtems_wifi_reg_read( RTEMS_WIFI_CLK_EN_REG );
  rtems_wifi_reg_write( RTEMS_WIFI_CLK_EN_REG,
                        reg & ~RTEMS_WIFI_CLK_BT_COMMON_M );
}

/*
 * The clocks register_chipv7_phy() needs, and which nothing else turns on.
 *
 * ESP-IDF does this in esp_phy_enable(): esp_phy_common_clock_enable() then
 * phy_module_enable(), both left ON, immediately before
 * esp_phy_load_cal_and_init() reaches register_chipv7_phy().  There is even an
 * assert between them -- phy_module_has_clock_bits() -- which is upstream
 * saying that calibrating without these is a programming error rather than a
 * degraded mode.
 *
 * This port skipped both, and QEMU did not care: the analog registers answer
 * whether or not anything is clocked.  Silicon does care, and the symptom is
 * ram_iq_est_enable spinning in its convergence loop until a watchdog fires.
 */
static void rtems_esp_wifi_clocks_on( void )
{
  uint32_t reg = rtems_wifi_reg_read( RTEMS_WIFI_CLK_EN_REG );

  rtems_wifi_reg_write(
    RTEMS_WIFI_CLK_EN_REG,
    reg | RTEMS_WIFI_CLK_BT_COMMON_M | RTEMS_WIFI_CLK_PHY_EN_M |
      RTEMS_WIFI_CLK_MAC_EN
  );
}

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

  /*
   * Before the PHY, not after.  register_chipv7_phy() calibrates against the
   * analog blocks, and on silicon they have to be powered and out of isolation
   * first or the calibration loops never converge.
   */
  printk( "rtems-esp-wifi: powering the modem domain...\n" );
  rtems_esp_wifi_power_domain_on();
  rtems_esp_wifi_clocks_on();
  /*
   * Let the internal I2C analog bus reach the RF blocks.
   *
   * ANA_CONFIG_REG bits [17:8] are a *disable* mask over the ten analog
   * targets the bus can address -- a bit set means that target is cut off --
   * and ANA_I2C_SAR_FORCE_PD/PU gate the SAR analog, which is the receive
   * ADC.  ESP-IDF opens these through regi2c_ctrl_ll_i2c_bbpll_enable() and
   * regi2c_ctrl_ll_i2c_sar_periph_enable(); nothing on this boot path does.
   *
   * It is the last thing between the FE and a working estimator.
   * ram_iq_est_enable() arms the estimator entirely through FE registers --
   * FE[0x140], FE[0x144], then it polls FE[0x174] bit 16 -- and those writes
   * demonstrably land, because the FE window answers 52 of 64 words after the
   * power-up.  What it is waiting on is the receive chain actually producing
   * samples, and that is analog: the RF synthesiser and the ADC, both reached
   * only over this bus.  With the bus masked off the digital side looks
   * perfect and the estimate never completes, which is exactly what the
   * hardware does.
   */
  {
    uint32_t reg = rtems_wifi_reg_read( RTEMS_WIFI_ANA_CONFIG_REG );

    printk( "rtems-esp-wifi: analog bus conf0 %08x config %08x config2 %08x\n",
            rtems_wifi_reg_read( RTEMS_WIFI_I2C_MST_ANA_CONF0_REG ),
            reg,
            rtems_wifi_reg_read( RTEMS_WIFI_ANA_CONFIG2_REG ) );

    reg &= ~RTEMS_WIFI_ANA_CONFIG_M;        /* every target reachable */
    reg &= ~RTEMS_WIFI_ANA_I2C_SAR_FORCE_PD;
    rtems_wifi_reg_write( RTEMS_WIFI_ANA_CONFIG_REG, reg );

    reg = rtems_wifi_reg_read( RTEMS_WIFI_ANA_CONFIG2_REG );
    rtems_wifi_reg_write( RTEMS_WIFI_ANA_CONFIG2_REG,
                          reg | RTEMS_WIFI_ANA_I2C_SAR_FORCE_PU );

    /*
     * And the system clock, which nothing on this boot path configures.
     * ESP-IDF's second-stage bootloader runs rtc_clk_init() and leaves the CPU
     * on the BBPLL at 160MHz with the crystal frequency recorded in
     * RTC_CNTL_STORE4; a direct-boot image has no second stage, so whatever
     * the ROM left is what the PHY gets.  SOC_CLK_SEL 0 means the CPU is still
     * on the crystal, and STORE4 not reading 40 in both halves means
     * rtc_clk_xtal_freq_get() cannot tell the PHY what the crystal is.
     */
    printk( "rtems-esp-wifi: sysclk %08x cpuperconf %08x xtalfreq %08x\n",
            rtems_wifi_reg_read( 0x600C0058u ),
            rtems_wifi_reg_read( 0x600C0008u ),
            rtems_wifi_reg_read( 0x600080B8u ) );

    printk( "rtems-esp-wifi: analog bus opened, config now %08x / %08x\n",
            rtems_wifi_reg_read( RTEMS_WIFI_ANA_CONFIG_REG ),
            rtems_wifi_reg_read( RTEMS_WIFI_ANA_CONFIG2_REG ) );

    rtems_esp_wifi_cpu_clock_to_pll();

    printk( "rtems-esp-wifi: cpu on pll, sysclk %08x cpuperconf %08x\n",
            rtems_wifi_reg_read( RTEMS_WIFI_SYSTEM_SYSCLK_CONF_REG ),
            rtems_wifi_reg_read( RTEMS_WIFI_SYSTEM_CPU_PER_CONF_REG ) );
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
  /*
   * The MAC goes into the calibration data before calibrating, which
   * esp_phy_load_cal_and_init() does and this port did not:
   *
   *     ESP_ERROR_CHECK( esp_efuse_mac_get_default( sta_mac ) );
   *     memcpy( cal_data->mac, sta_mac, 6 );
   *
   * Read straight from eFuse rather than through esp_wifi_get_mac(), because
   * this runs before the WiFi libraries are initialised and there is nothing
   * to ask.  Byte order is the same trap as in the OS adapter's _read_mac:
   * mac[0] is the MOST significant byte, opposite to the two eFuse words.
   */
  {
    uint32_t mac0 = rtems_wifi_reg_read( RTEMS_WIFI_EFUSE_MAC0_REG );
    uint32_t mac1 = rtems_wifi_reg_read( RTEMS_WIFI_EFUSE_MAC1_REG );

    rtems_wifi_cal_data.mac[ 0 ] = (uint8_t) ( ( mac1 >> 8 ) & 0xffu );
    rtems_wifi_cal_data.mac[ 1 ] = (uint8_t) ( mac1 & 0xffu );
    rtems_wifi_cal_data.mac[ 2 ] = (uint8_t) ( ( mac0 >> 24 ) & 0xffu );
    rtems_wifi_cal_data.mac[ 3 ] = (uint8_t) ( ( mac0 >> 16 ) & 0xffu );
    rtems_wifi_cal_data.mac[ 4 ] = (uint8_t) ( ( mac0 >> 8 ) & 0xffu );
    rtems_wifi_cal_data.mac[ 5 ] = (uint8_t) ( mac0 & 0xffu );

    printk( "rtems-esp-wifi: calibrating for MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            rtems_wifi_cal_data.mac[ 0 ], rtems_wifi_cal_data.mac[ 1 ],
            rtems_wifi_cal_data.mac[ 2 ], rtems_wifi_cal_data.mac[ 3 ],
            rtems_wifi_cal_data.mac[ 4 ], rtems_wifi_cal_data.mac[ 5 ] );
  }

#ifdef RTEMS_WIFI_DEBUG_NO_WDT
  rtems_esp_wifi_watchdogs_off();
#endif

  /* Before calibration, as esp_phy_load_cal_and_init() does. */
  phy_bbpll_en_usb( true );

  printk( "rtems-esp-wifi: register_chipv7_phy( PHY_RF_CAL_FULL )...\n" );

  phy_result = register_chipv7_phy(
    &phy_init_data,
    &rtems_wifi_cal_data,
    RTEMS_WIFI_CAL_MODE
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

#ifdef RTEMS_WIFI_DEBUG_PROBE
  /*
   * hal_init() sets bit 1 of MAC[0x60033d14] and spins until bit 0 reads back
   * set -- a request/acknowledge for MAC bring-up that silicon never
   * acknowledges, though QEMU's model does.  Whether the MAC window answers
   * the bus at all decides whether that is a dead block or a live one
   * declining to finish.
   */
  rtems_esp_wifi_probe_window( "MAC  ", 0x60033000u, 64u );
  rtems_esp_wifi_probe_window( "MACd ", 0x60033d00u, 32u );
  printk( "rtems-esp-wifi: mac[0d14] %08x clk_en %08x rst_en %08x\n",
          rtems_wifi_reg_read( 0x60033D14u ),
          rtems_wifi_reg_read( RTEMS_WIFI_CLK_EN_REG ),
          rtems_wifi_reg_read( RTEMS_WIFI_RST_EN_REG ) );

#endif

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
