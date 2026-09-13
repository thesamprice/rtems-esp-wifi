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
 * ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE.
 */

/*
 * The part of the WiFi glue that needs nothing from ESP-IDF.
 *
 * docs/rom-step.md lists the 42 symbols left once the ROM linker scripts are
 * included.  These twelve are the ones that can be written against RTEMS
 * alone: six logging hooks, a critical section, and two event-base names.
 * The rest need esp_phy's calibration tables or esp_wifi's own C, which is
 * why they are not here.
 *
 * Written first because they are the only part that can be compiled and
 * checked before the components build, and because getting them out of the
 * list makes what remains easier to see.
 */

/* For TickType_t and the vTaskDelay declaration this file implements. */
#include <freertos/task.h>

#include <rtems.h>
#include <rtems/bspIo.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>

/*
 * Logging.
 *
 * ESP-IDF routes these through ESP_LOGI, which reaches esp_log's tag
 * filtering and its own vprintf hook.  Here they go to printk: the callers
 * are the WiFi lower half, which prints from its own task and from interrupt
 * context, and printk is the one output path on RTEMS that does not take a
 * lock or allocate.
 *
 * The trailing-whitespace trim is ESP-IDF's, kept because the blobs emit
 * messages that end in a newline and printk adds nothing -- without it every
 * line arrives doubled.
 */
#define LIB_PRINTF_BUFFER 96

static int rtems_esp_lib_printf(const char *tag, const char *format, va_list ap)
{
  char buf[LIB_PRINTF_BUFFER];
  int len;
  int i;

  len = vsnprintf(buf, sizeof(buf) - 1, format, ap);
  buf[sizeof(buf) - 1] = '\0';

  for (i = len - 1; i >= 0; --i) {
    if (buf[i] != '\n' && buf[i] != '\r' && buf[i] != ' ') {
      break;
    }
    buf[i] = '\0';
  }

  if (i > 0) {
    printk("%s: %s\n", tag, buf);
  }

  return len;
}

#define RTEMS_ESP_PRINTF(name, tag)                     \
  int name(const char *format, ...)                     \
  {                                                     \
    va_list ap;                                         \
    int len;                                            \
                                                        \
    va_start(ap, format);                               \
    len = rtems_esp_lib_printf(tag, format, ap);        \
    va_end(ap);                                         \
                                                        \
    return len;                                         \
  }

RTEMS_ESP_PRINTF(pp_printf, "wifi.pp")
RTEMS_ESP_PRINTF(phy_printf, "wifi.phy")
RTEMS_ESP_PRINTF(net80211_printf, "wifi.net80211")
RTEMS_ESP_PRINTF(mesh_printf, "wifi.mesh")
RTEMS_ESP_PRINTF(sc_printf, "wifi.smartconfig")
RTEMS_ESP_PRINTF(coex_pti_print, "wifi.coex")

/*
 * The PHY's critical section.
 *
 * libphy calls this around register sequences that must not be interrupted,
 * from task and interrupt context both, and it nests.  So it is an interrupt
 * lock rather than a mutex: a mutex cannot be taken from an ISR, and
 * disabling interrupts unconditionally on exit would re-enable them inside an
 * outer critical section.
 *
 * RTEMS' interrupt lock returns the previous level through a context object
 * rather than as a value, and ESP-IDF's signature returns a uint32_t.  The
 * context is kept here rather than handed out, which is sound because the
 * calls nest rather than interleave -- libphy always exits the section it
 * entered last, and the level it passes back is the one it was given.
 */
RTEMS_INTERRUPT_LOCK_DEFINE( static, rtems_esp_phy_lock, "ESP PHY" )

static rtems_interrupt_lock_context rtems_esp_phy_lock_context;

uint32_t phy_enter_critical(void)
{
  rtems_interrupt_lock_acquire(
    &rtems_esp_phy_lock,
    &rtems_esp_phy_lock_context
  );

  /*
   * The value is opaque to the caller: libphy passes back whatever it was
   * given.  Zero rather than the saved level, because the level lives in the
   * context above and handing out a copy would invite someone to trust it.
   */
  return 0;
}

void phy_exit_critical(uint32_t level)
{
  (void) level;

  rtems_interrupt_lock_release(
    &rtems_esp_phy_lock,
    &rtems_esp_phy_lock_context
  );
}

/*
 * Event base names.
 *
 * ESP_EVENT_DEFINE_BASE(id) expands to `esp_event_base_t const id = #id`, and
 * esp_event_base_t is a const char *.  Defined here rather than pulled from
 * esp_event's C so that the blobs link before that component does; when
 * esp_event is built for RTEMS these two move there and come out of here.
 */
const char *const WIFI_EVENT = "WIFI_EVENT";
const char *const SC_EVENT = "SC_EVENT";

/* === randomness ======================================================== */

/*
 * esp_hw_support's public randomness API.  On hardware these read the RNG
 * register, which mixes the SAR ADC and the RC fast clock and is only a real
 * random source while the radio is on.
 *
 * Here they are getentropy(), which is the same source the OS adapter's _rand
 * entry uses -- on this board bsps/shared/dev/getentropy/getentropy-cpucounter.c,
 * which is a CPU counter and says so in its own header.  That is weak, and it
 * is worth being precise about the consequence: the supplicant uses os_random()
 * for nonces, so on a board whose getentropy is the counter fallback, WPA
 * nonces are predictable to anyone who knows the boot timing.  Good enough to
 * associate and bring a station up, which is what this port is for; not good
 * enough to deploy.
 *
 * A BSP with a real entropy source needs no change here -- getentropy() is the
 * seam, and RTEMS picks the implementation per BSP.
 */
uint32_t esp_random( void )
{
  uint32_t value;

  if ( getentropy( &value, sizeof( value ) ) != 0 ) {
    value = (uint32_t) rtems_clock_get_uptime_nanoseconds();
  }

  return value;
}

void esp_fill_random( void *buf, size_t len )
{
  if ( buf == NULL || len == 0 ) {
    return;
  }

  if ( getentropy( buf, len ) != 0 ) {
    /*
     * Fill rather than leave the caller's buffer untouched.  A partially
     * written key buffer is worse than a weak one, because the caller has no
     * way to tell.
     */
    unsigned char *p = buf;
    size_t         i;

    for ( i = 0; i < len; ++i ) {
      p[ i ] = (unsigned char) esp_random();
    }
  }
}

/* === the supplicant's clock ============================================ */

/*
 * os_get_time() for wpa_supplicant.
 *
 * Why this is here at all: the blobs want hexstr2bin, which lives in the
 * supplicant's src/utils/common.c, and that object also contains
 * wpa_get_ntp_timestamp() -- the one caller of os_get_time().  Nothing on the
 * station path calls it, but a reference in the object is a reference at link
 * time regardless.
 *
 * The supplicant's own port/os_xtensa.c has this function and is identical to
 * it (gettimeofday into the two fields).  Compiling that file instead needs
 * mbedtls configured, for an #include serving a forced_memzero() that is
 * behind CONFIG_CRYPTO_MBEDTLS and not compiled -- disproportionate for six
 * lines.
 *
 * Weak, so when the supplicant comes in properly its strong definition wins
 * with no change here and no duplicate-symbol error.  struct os_time is laid
 * out by port/include/os.h as { os_time_t sec; suseconds_t usec; }; it is
 * declared locally rather than by including os.h, because os.h drags in the
 * supplicant's whole header chain into a file that otherwise needs none of it.
 */
struct rtems_esp_os_time {
  time_t      sec;
  suseconds_t usec;
};

int __attribute__((weak)) os_get_time( struct rtems_esp_os_time *t )
{
  struct timeval tv;
  int            rv;

  rv = gettimeofday( &tv, NULL );

  t->sec  = tv.tv_sec;
  t->usec = tv.tv_usec;

  return rv;
}

/* === the one FreeRTOS call that is not behind the adapter ============== */

/*
 * wpa_supplicant's port/eloop.c calls vTaskDelay() directly rather than going
 * through wifi_osi_funcs_t, so the port has to provide it.  See the comment on
 * the declaration in include/freertos/task.h for why that is not a shim.
 *
 * FreeRTOS counts in ticks and so does rtems_task_wake_after(), so this is a
 * unit-for-unit call rather than a conversion.  Zero means "yield" in
 * FreeRTOS; RTEMS spells that RTEMS_YIELD_PROCESSOR, which is also 0, so the
 * two agree without a special case -- but it is written out because relying on
 * two constants happening to match is how the queue-return-convention bug got
 * in.
 */
void vTaskDelay( TickType_t ticks )
{
  (void) rtems_task_wake_after(
    ticks == 0 ? RTEMS_YIELD_PROCESSOR : (rtems_interval) ticks
  );
}
