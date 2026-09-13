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
 * The lwIP netif that carries WiFi frames, and the one indirection under it.
 *
 * Two things live here and they are deliberately separate:
 *
 *   * rtems_esp_netif_driver -- five function pointers that are everything the
 *     netif needs from whatever is driving the radio.  Nothing in this struct
 *     mentions esp_wifi, and that is the point: rtems-esphome#99 records that
 *     the blob route can never be proposed to upstream RTEMS and that ESP32
 *     Open MAC is the only route that could, so a different driver underneath
 *     has to be a substitution rather than a rewrite.  An implementation needs
 *     lwIP (it is an lwIP netif) and nothing from Espressif's headers.
 *
 *   * the netif itself, which is one instance: a station.  See
 *     docs/step8-netif.md for what a second, AP-mode instance would take.
 *
 * The interesting half of the contract is the RX buffer handle, called `eb`
 * because that is what esp_private/wifi.h calls it.  Read
 * rtems_esp_netif_input() below and the long comment on it in
 * src/rtems_esp_netif.c before touching either side: a handle that is not
 * returned exhausts the driver's RX pool, and the symptom is the radio going
 * quiet after N frames rather than any kind of error.
 */

#ifndef RTEMS_ESP_NETIF_H
#define RTEMS_ESP_NETIF_H

#include <stdbool.h>
#include <stdint.h>

#include <lwip/err.h>
#include <lwip/netif.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What the netif needs from the driver below it.
 *
 * Every entry returning int returns zero for success and anything else for
 * failure; the value is only reported, never interpreted, so a driver may
 * return its own error codes.  Zero is chosen rather than esp_err_t precisely
 * so that this header does not drag in esp_err.h and with it the whole
 * ESP-IDF include path -- a driver that has nothing to do with ESP-IDF should
 * not have to compile against it.
 *
 * All of the entries are mandatory except release_rx, which may be NULL for a
 * driver whose received frames carry no handle to give back.
 */
typedef struct {
  /* Named in messages, so that a log line says which driver spoke. */
  const char *name;

  /*
   * Install the receive path.  The driver arranges for
   * rtems_esp_netif_input() to be called once per received frame, in a task
   * context in which lwIP's thread-safe entry points may be used, and returns
   * zero if it managed that.
   *
   * It is a separate call rather than part of netif creation because the
   * driver may not be able to accept it until the radio is started; the blob
   * driver registers from the WIFI_EVENT_STA_START handler for that reason.
   */
  int ( *attach )( void );

  /* Remove the receive path again.  May be NULL. */
  int ( *detach )( void );

  /*
   * Send one complete Ethernet frame, header included, from a single
   * contiguous buffer.  The netif guarantees contiguity; it linearises a pbuf
   * chain first if it has to.
   *
   * The driver does not take ownership: the buffer is the netif's and is
   * reused as soon as this returns, so a driver that cannot send
   * synchronously must copy.  esp_wifi_internal_tx() copies, which is why the
   * blob driver needs nothing further.
   */
  int ( *transmit )( void *frame, uint16_t len );

  /*
   * Give back the handle that came with a received frame.  Called exactly once
   * per rtems_esp_netif_input(), from inside it, on every path.  May be NULL
   * if the driver passes no handle.
   */
  void ( *release_rx )( void *eb );

  /* The station's MAC address, for netif->hwaddr. */
  int ( *get_mac )( uint8_t mac[ 6 ] );
} rtems_esp_netif_driver;

/*
 * The driver over Espressif's WiFi libraries: esp_wifi_internal_tx(),
 * esp_wifi_internal_reg_rxcb() and esp_wifi_internal_free_rx_buffer(), bound
 * to WIFI_IF_STA.
 */
extern const rtems_esp_netif_driver rtems_esp_netif_blob_driver;

/*
 * Counters, and they are here rather than being a debugging afterthought.
 *
 * eb_taken and eb_released are the RX buffer contract made observable: they
 * must be equal whenever no rtems_esp_netif_input() call is in progress, and a
 * test can assert that where it cannot assert that packets moved.  On
 * hardware, a leak shows up as the difference growing; in emulation, where no
 * frame can arrive at all, both stay zero and the assertion passes without
 * having tested anything -- so read rx_frames first and treat a zero there as
 * "this proved nothing".
 */
typedef struct {
  uint32_t rx_frames;
  uint32_t rx_bytes;
  uint32_t rx_dropped_no_pbuf;
  uint32_t rx_dropped_stack_busy;
  uint32_t rx_dropped_bad_frame;
  uint32_t eb_taken;
  uint32_t eb_released;
  uint32_t tx_frames;
  uint32_t tx_bytes;
  uint32_t tx_linearised;
  uint32_t tx_dropped_too_long;
  uint32_t tx_dropped_driver;
  uint32_t link_up;
  uint32_t link_down;
} rtems_esp_netif_stats;

/*
 * Create the station netif over `driver` and register the WiFi event handlers
 * that drive its link state.  The address arguments are the usual lwIP ones
 * and may all be NULL, which is what to pass when DHCP is to supply them.
 *
 * `netif` is the storage lwIP will use, and it must outlive the interface --
 * lwIP keeps the pointer, it does not copy.  NULL asks this file for its own,
 * which is what a caller that has no opinion should pass.  It is a parameter
 * rather than always this file's static because rtems-lwip's start_networking()
 * hands its caller's netif down through esp32c3_netif_add(), and an
 * application that follows that convention -- checking netif_is_up() on the
 * netif it passed, as tests/zynq-lwip does -- has to be given the one that was
 * really added rather than a zeroed bystander.
 *
 * tcpip_init() must already have been called -- this does not call it, because
 * whether the application or an rtems-lwip start_networking() owns that is the
 * application's decision and calling it twice is not harmless.
 *
 * Returns the netif, or NULL with the reason reported on the console.
 */
struct netif *rtems_esp_netif_add(
  struct netif                 *netif,
  const rtems_esp_netif_driver *driver,
  const ip4_addr_t             *ipaddr,
  const ip4_addr_t             *netmask,
  const ip4_addr_t             *gateway
);

/* The netif created above, or NULL. */
struct netif *rtems_esp_netif_get( void );

/*
 * Install the driver's receive path and bring the interface administratively
 * up.  The WIFI_EVENT_STA_START handler calls this; it is exported so that a
 * test can call it directly and check the return value, which is one of the
 * few things about the RX path that can be checked without a radio.
 */
int rtems_esp_netif_start( void );

/* The inverse.  Removes the receive path and takes the interface down. */
int rtems_esp_netif_stop( void );

/*
 * Hand one received Ethernet frame to lwIP.  The driver calls this; nothing
 * else should.
 *
 * `frame`/`len` are borrowed for the duration of the call only.  `eb` is the
 * driver's handle for the frame, or NULL, and this function returns it through
 * the driver's release_rx on every path including every error path -- that is
 * the whole reason the release does not live in the driver.
 */
void rtems_esp_netif_input( void *frame, uint16_t len, void *eb );

/* Live counters.  Never NULL. */
const rtems_esp_netif_stats *rtems_esp_netif_get_stats( void );

/* Print them, one per line, as `name value`. */
void rtems_esp_netif_print_stats( void );

/*
 * rtems-lwip's hook, defined here so that linking this file overrides it.
 *
 * rtemslwip/esp32c3/netstart.c's start_networking() brings the stack up and
 * then calls this to get an interface, because the C3 has no Ethernet
 * controller and its only one is the radio, which rtems-lwip must not know
 * about.  Its own definition is weak and does nothing but explain its absence.
 *
 * The prototype is repeated here rather than taken from a header because
 * rtems-lwip does not declare it in netstart.h -- there is nothing there for
 * an application to call.  It is declared at all so that a build of this file
 * without liblwip on the link line still type-checks the override, which is
 * the failure worth catching early: a signature that drifts from the weak
 * definition would link cleanly and pass the wrong arguments.
 *
 * Returns zero on success, matching the weak default's contract, which is the
 * opposite of everything else in rtems-lwip and is checked against the real
 * source rather than remembered.
 */
int esp32c3_netif_add(
  struct netif  *net_interface,
  ip_addr_t     *ipaddr,
  ip_addr_t     *netmask,
  ip_addr_t     *gateway,
  unsigned char *mac_address
);

#ifdef __cplusplus
}
#endif

#endif /* RTEMS_ESP_NETIF_H */
