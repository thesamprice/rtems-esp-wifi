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
 * The netif: where WiFi frames meet rtems-lwip.
 *
 * ESP-IDF puts this in esp_netif, and esp_netif is deliberately not in
 * esp-hal-3rdparty -- the branch ships 100 components and the netif
 * abstraction is not one of them, because a third-party framework is expected
 * to bring its own stack.  NuttX references it nowhere.  So this file is the
 * port's own, and it is an lwIP netif of the same shape as rtems-lwip's Zynq
 * Cadence GEM driver: netif_add() with tcpip_input, a linkoutput, and a
 * receive path that calls netif->input().
 *
 * The file has two halves, separated by a comment band:
 *
 *   1. the netif, which knows about lwIP and about rtems_esp_netif_driver;
 *   2. the blob driver, which knows about esp_wifi_internal_* and is the only
 *      part a substitution replaces.
 *
 * Nothing in half 1 mentions esp_wifi, on purpose.  rtems-esphome#99 records
 * why: the blob route cannot be proposed to upstream RTEMS -- eight binary
 * archives and a 120-entry table shimming someone else's OS abstraction --
 * and ESP32 Open MAC is the only route that could.  It is not a migration
 * anyone should attempt now (their driver is ESP32/Xtensa, not C3, and it
 * still needs the blobs for PHY bring-up), so the useful thing is not to
 * foreclose it.  One indirection does that; a second would be decoration.
 *
 * ESP32 Open MAC's own lwIP integration was read before this was written,
 * because they solved the same problem against the same TX/RX primitives.
 * Two things were taken from it and one was not:
 *
 *   * taken: their receive entry point takes the frame *and* a separate handle
 *     for the buffer it lives in (esp_netif_receive(netif, buf, len, buf)),
 *     and the free of that handle is a driver callback rather than something
 *     the receive path does itself.  The same split is here, as
 *     rtems_esp_netif_input()'s `eb` and the driver's release_rx.
 *   * taken: their transmit is documented as not taking ownership -- "you're
 *     allowed to reuse the buffer directly after this returns" -- and so is
 *     ours, which is what lets the chained-pbuf case use one staging buffer.
 *   * not taken: they hand the buffer to ESP-IDF's esp_netif, which defers the
 *     free until lwIP is finished with a zero-copy pbuf that references it.
 *     This copies and frees immediately.  See the ownership comment on
 *     rtems_esp_netif_input().
 */

/* Before any esp_private header: phy.h names _lock_t, which RTEMS' newlib
 * spells _LOCK_T. */
#include <rtems-esp/newlib-compat.h>

#include <rtems-esp/netif.h>

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_private/wifi.h>

#include <lwip/etharp.h>
#include <lwip/ethip6.h>
#include <lwip/pbuf.h>
#include <lwip/tcpip.h>
#include <netif/ethernet.h>

#include <rtems.h>
#include <rtems/bspIo.h>

#include <string.h>

#ifndef ESP_OK
#define ESP_OK 0
#endif

/*
 * 1518 is 1500 of payload, 14 of Ethernet header and 4 for a VLAN tag that
 * lwIP will insert here rather than in the pbuf when ETHARP_SUPPORT_VLAN is
 * on.  The netif's MTU is set from the first of those three numbers, so a
 * frame longer than this cannot arrive from lwIP; the length is still checked,
 * because the check is one comparison and the alternative is a stack overrun
 * driven by whatever configured the MTU.
 */
#define RTEMS_ESP_NETIF_MTU       1500
#define RTEMS_ESP_NETIF_FRAME_MAX 1518

/* "wl0", following lwIP's two-letter-plus-number convention. */
#define RTEMS_ESP_NETIF_NAME0 'w'
#define RTEMS_ESP_NETIF_NAME1 'l'

/*
 * One netif, a station.
 *
 * Static rather than allocated, and singular rather than an array, for a
 * reason that is a property of the seam and not a simplification:
 * esp_wifi_internal_reg_rxcb() takes a bare function pointer with no context
 * argument -- "Currently we support only one RX callback for each interface",
 * says the header -- so the callback has to find its netif through a static
 * whatever else happens.  Given that, a second interface is a second static
 * and a second trampoline, not a data structure.  docs/step8-netif.md says
 * what AP mode would take.
 *
 * It is a static *pointer* to storage the caller may supply, rather than
 * static storage outright, and the reason is rtems-lwip: start_networking()
 * passes the application's own netif down through esp32c3_netif_add(), and an
 * application that then asks netif_is_up() about the netif it handed over has
 * to be asking about the one that was really added.  rtems_esp_netif_own is
 * what a caller with no opinion gets.  The argument above is untouched by
 * this: there is still exactly one station and the callback still finds it
 * through a static.
 */
static struct netif                  rtems_esp_netif_own;
static struct netif                 *rtems_esp_netif;
static const rtems_esp_netif_driver *rtems_esp_netif_driver_ops;
static bool                          rtems_esp_netif_added;
static bool                          rtems_esp_netif_attached;

static rtems_esp_netif_stats rtems_esp_netif_counters;

/*
 * The staging buffer for a chained pbuf on transmit, and its lock.
 *
 * A lock is needed because linkoutput is not reached from one thread only.
 * With LWIP_TCPIP_CORE_LOCKING -- which rtems-lwip leaves at lwIP's default of
 * 1, and which a config.ini entry can turn off -- an application thread in
 * sockets takes the core lock and runs the output path itself, so which
 * threads reach here is a configuration property of the stack rather than
 * something a driver may assume.  Two frames interleaved in one buffer would
 * go out as two corrupt frames and nothing would report an error, so this does
 * not rely on the core lock being held.
 *
 * A mutex rather than an interrupt lock: this is task context and it is held
 * across a memcpy of up to 1518 bytes plus the driver's transmit.
 */
static uint8_t   rtems_esp_netif_staging[ RTEMS_ESP_NETIF_FRAME_MAX ];
static rtems_id  rtems_esp_netif_staging_lock;

/* ------------------------------------------------------------------------ *
 *                                the netif                                 *
 * ------------------------------------------------------------------------ */

/*
 * Transmit.
 *
 * The driver wants one contiguous buffer and lwIP may hand over a chain --
 * LWIP_NETIF_TX_SINGLE_PBUF defaults to 0 and rtems-lwip does not set it, and
 * a TCP segment is normally a header pbuf followed by a data pbuf, so the
 * chained case is the common one and not the exception.  A single pbuf is sent
 * from its own payload, which costs nothing; a chain is linearised.
 *
 * No completion handling, because the driver contract says the buffer is
 * borrowed for the duration of the call.  That is true of
 * esp_wifi_internal_tx(), whose header states it "makes a copy of the input
 * buffer and then forwards the buffer copy to WiFi driver".  The alternative
 * primitive, esp_wifi_internal_tx_by_ref(), hands the pbuf itself down with a
 * reference count and a pair of callbacks registered through
 * esp_wifi_internal_reg_netstack_buf_cb(); it saves the copy and buys a second
 * buffer-ownership contract with a second way to leak.  One is enough to get
 * right, and this one cannot be avoided.
 */
static err_t rtems_esp_netif_linkoutput( struct netif *netif, struct pbuf *p )
{
  void     *frame;
  uint16_t  len;
  int       rv;

  (void) netif;

  if ( rtems_esp_netif_driver_ops == NULL ) {
    return ERR_IF;
  }

  if ( p->tot_len > RTEMS_ESP_NETIF_FRAME_MAX ) {
    ++rtems_esp_netif_counters.tx_dropped_too_long;

    return ERR_MEM;
  }

  /* tot_len is a u16_t, and the check above bounds it well below the cast. */
  len = (uint16_t) p->tot_len;

  if ( p->next == NULL ) {
    frame = p->payload;
  } else {
    rtems_status_code sc = rtems_semaphore_obtain(
      rtems_esp_netif_staging_lock,
      RTEMS_WAIT,
      RTEMS_NO_TIMEOUT
    );

    if ( sc != RTEMS_SUCCESSFUL ) {
      ++rtems_esp_netif_counters.tx_dropped_driver;

      return ERR_IF;
    }

    if ( pbuf_copy_partial( p, rtems_esp_netif_staging, len, 0 ) != len ) {
      rtems_semaphore_release( rtems_esp_netif_staging_lock );
      ++rtems_esp_netif_counters.tx_dropped_driver;

      return ERR_BUF;
    }

    ++rtems_esp_netif_counters.tx_linearised;
    frame = rtems_esp_netif_staging;
  }

  rv = rtems_esp_netif_driver_ops->transmit( frame, len );

  if ( p->next != NULL ) {
    rtems_semaphore_release( rtems_esp_netif_staging_lock );
  }

  if ( rv != 0 ) {
    /*
     * Counted, not printed.  Transmit fails routinely and for ordinary
     * reasons -- not associated yet, TX disallowed before authentication --
     * and a printk per dropped frame from the output path turns a slow
     * association into a console flood that hides whatever came next.
     */
    ++rtems_esp_netif_counters.tx_dropped_driver;

    return ERR_IF;
  }

  ++rtems_esp_netif_counters.tx_frames;
  rtems_esp_netif_counters.tx_bytes += len;

  return ERR_OK;
}

/*
 * Receive, and this is the function to read twice.
 *
 * THE BUFFER OWNERSHIP CONTRACT
 *
 * The driver hands over a frame and, separately, a handle to whatever holds
 * it.  For the blob driver that handle is esp_private/wifi.h's `eb`, and it
 * must go back through esp_wifi_internal_free_rx_buffer().  The pool it comes
 * from is finite and sized by CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM (10) and
 * CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM (32) in include/rtems-esp/sdkconfig.h,
 * so a handle that is not returned is not an error anywhere -- it is ten or
 * thirty-two frames of normal operation followed by a radio that has gone
 * quiet, diagnosed days later.  That is why the release is here, in the netif,
 * and not in the driver: one call site, reached on every path.
 *
 * The function is written as three steps in this order and the order is the
 * contract:
 *
 *   1. copy the frame into a pbuf, which may fail;
 *   2. release the handle -- unconditionally, with no return between here and
 *      step 1, which is why the validation above is an `if` and not an early
 *      return;
 *   3. hand the pbuf up, which may also fail, by which time the handle is
 *      already back.
 *
 * WHY COPY
 *
 * lwIP can wrap a foreign buffer instead: pbuf_alloced_custom() with PBUF_REF
 * and a custom free that returns the handle.  That is what ESP-IDF's wlanif
 * does when CONFIG_LWIP_L2_TO_L3_COPY is off, and it is what ESP32 Open MAC
 * gets by going through esp_netif.  It saves a memcpy of at most 1518 bytes
 * and costs the one thing this file exists to get right: the handle's lifetime
 * stops being the body of this function and becomes "until the tcpip thread
 * frees a pbuf", so the release moves to a callback that must also fire on
 * lwIP's internal error paths, and the leak becomes invisible again.  The copy
 * is the reason the invariant above can be stated in three lines.
 *
 * WHY netif->input AND NOT ANYTHING ELSE
 *
 * netif->input is tcpip_input, set by netif_add() below.  With
 * LWIP_TCPIP_CORE_LOCKING_INPUT at lwIP's default of 0, tcpip_input allocates
 * a message, posts it to the tcpip thread and returns, so the IP stack runs
 * there and not here.
 *
 * That matters because of the context this runs in.  The blob driver's
 * registered callback is invoked from the WiFi task -- the one the OS adapter
 * creates through wifi_osi_funcs_t -- not from an interrupt handler.  So
 * blocking is permitted, which is why pbuf_alloc() is allowed at all, and
 * spending time is not: running ethernet_input, ip4_input, tcp_input and an
 * application's socket wake-up on the WiFi task means the radio is not being
 * serviced for the duration, and means lwIP can re-enter esp_wifi from inside
 * the WiFi task when a response goes out.  netif_input() -- lwIP's other entry
 * point, for NO_SYS builds -- does exactly that, and is the wrong one here for
 * that reason.  rtems-lwip's Zynq GEM driver passes tcpip_input to netif_add()
 * and calls netif->input() from a thread, and this is the same arrangement
 * with the WiFi task in place of its xemacif_input_thread.
 *
 * Calling netif->input rather than tcpip_input directly keeps the choice in
 * the netif_add() call, which is where a reader of an lwIP driver looks for
 * it.
 */
void rtems_esp_netif_input( void *frame, uint16_t len, void *eb )
{
  struct pbuf *p = NULL;

  ++rtems_esp_netif_counters.eb_taken;

  if (
    frame != NULL
      && len != 0
      && len <= RTEMS_ESP_NETIF_FRAME_MAX
      && rtems_esp_netif_added
  ) {
    p = pbuf_alloc( PBUF_RAW, len, PBUF_POOL );

    if ( p != NULL && pbuf_take( p, frame, len ) != ERR_OK ) {
      pbuf_free( p );
      p = NULL;
    }
  } else {
    ++rtems_esp_netif_counters.rx_dropped_bad_frame;
  }

  /*
   * Step 2.  The single release, and the reason there is no `return` above.
   */
  if ( eb != NULL && rtems_esp_netif_driver_ops != NULL
       && rtems_esp_netif_driver_ops->release_rx != NULL ) {
    rtems_esp_netif_driver_ops->release_rx( eb );
  }

  ++rtems_esp_netif_counters.eb_released;

  if ( p == NULL ) {
    ++rtems_esp_netif_counters.rx_dropped_no_pbuf;

    return;
  }

  if ( rtems_esp_netif->input( p, rtems_esp_netif ) != ERR_OK ) {
    /*
     * tcpip_input owns the pbuf only once it has posted it; on ERR_MEM -- no
     * message from MEMP_TCPIP_MSG_INPKT, or a full TCPIP_MBOX_SIZE mailbox --
     * the pbuf is still ours and leaks if we do not free it.  lwIP's own
     * drivers all carry this line for the same reason.
     */
    pbuf_free( p );
    ++rtems_esp_netif_counters.rx_dropped_stack_busy;

    return;
  }

  ++rtems_esp_netif_counters.rx_frames;
  rtems_esp_netif_counters.rx_bytes += len;
}

/*
 * netif_add()'s init callback, run with the netif zeroed.
 */
static err_t rtems_esp_netif_init( struct netif *netif )
{
  uint8_t mac[ ETH_HWADDR_LEN ];

  netif->name[ 0 ]  = RTEMS_ESP_NETIF_NAME0;
  netif->name[ 1 ]  = RTEMS_ESP_NETIF_NAME1;
  netif->output     = etharp_output;
  netif->linkoutput = rtems_esp_netif_linkoutput;
#if LWIP_IPV6
  netif->output_ip6 = ethip6_output;
#endif
  netif->mtu        = RTEMS_ESP_NETIF_MTU;
  netif->hwaddr_len = ETH_HWADDR_LEN;

  /*
   * NETIF_FLAG_LINK_UP is deliberately absent, which is where this parts
   * company with the GEM driver: that one sets it in its init because an
   * Ethernet MAC has no better information until its PHY thread runs, whereas
   * association is an event the WiFi libraries report.  Starting link-down and
   * raising it on WIFI_EVENT_STA_CONNECTED is what makes DHCP begin at the
   * right moment instead of at boot.
   */
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
#if LWIP_IGMP
  netif->flags |= NETIF_FLAG_IGMP;
#endif
#if LWIP_IPV6 && LWIP_IPV6_MLD
  netif->flags |= NETIF_FLAG_MLD6;
#endif

  memset( mac, 0, sizeof( mac ) );

  if ( rtems_esp_netif_driver_ops->get_mac( mac ) != 0 ) {
    /*
     * Reported and then carried on with, because an all-zero hwaddr is a
     * visible symptom -- ARP never resolves -- whereas returning ERR_IF here
     * loses the netif entirely and with it the message saying why.
     */
    printk( "rtems-esp-netif: %s cannot report a MAC address\n",
            rtems_esp_netif_driver_ops->name );
  }

  memcpy( netif->hwaddr, mac, ETH_HWADDR_LEN );

  return ERR_OK;
}

/*
 * Link state changes are asked for from the event task, and lwIP's core is not
 * hers to touch.
 *
 * netif_set_link_up() walks the netif's client data and, with LWIP_DHCP on,
 * calls into dhcp_network_changed_link_up(), so it is core work and belongs on
 * the tcpip thread or under the core lock.  The event handlers below run on
 * the event task from src/rtems_esp_event.c, which is neither.  tcpip_callback
 * runs the function on the tcpip thread whatever LWIP_TCPIP_CORE_LOCKING is
 * set to, so it is the answer that does not depend on a configuration this
 * file cannot see.
 *
 * The vendored Xilinx code in rtems-lwip does call netif_set_link_up()
 * straight from its own link_detect_thread, and that is not a precedent worth
 * following -- it is unsynchronised access to the stack that happens to be
 * rare enough not to have been noticed.
 */
static void rtems_esp_netif_do_link_up( void *ctx )
{
  (void) ctx;
  netif_set_link_up( rtems_esp_netif );
}

static void rtems_esp_netif_do_link_down( void *ctx )
{
  (void) ctx;
  netif_set_link_down( rtems_esp_netif );
}

static void rtems_esp_netif_do_up( void *ctx )
{
  (void) ctx;
  netif_set_up( rtems_esp_netif );
}

static void rtems_esp_netif_do_down( void *ctx )
{
  (void) ctx;
  netif_set_down( rtems_esp_netif );
}

static void rtems_esp_netif_core_call(
  tcpip_callback_fn fn,
  const char       *what
)
{
  err_t err;

  if ( !rtems_esp_netif_added ) {
    return;
  }

  err = tcpip_callback( fn, NULL );

  if ( err != ERR_OK ) {
    printk( "rtems-esp-netif: %s did not reach the stack (%i)\n",
            what, (int) err );
  }
}

int rtems_esp_netif_start( void )
{
  int rv;

  if ( !rtems_esp_netif_added || rtems_esp_netif_driver_ops == NULL ) {
    printk( "rtems-esp-netif: start before the netif was created\n" );

    return -1;
  }

  if ( rtems_esp_netif_attached ) {
    return 0;
  }

  rv = rtems_esp_netif_driver_ops->attach();

  if ( rv != 0 ) {
    /*
     * Loud, because this is the failure that otherwise presents as "the link
     * came up and no packet ever arrived".  A receive path that was never
     * installed produces no error of its own at any later point.
     */
    printk( "rtems-esp-netif: %s would not take the receive path (%i)\n",
            rtems_esp_netif_driver_ops->name, rv );

    return rv;
  }

  rtems_esp_netif_attached = true;
  rtems_esp_netif_core_call( rtems_esp_netif_do_up, "interface up" );

  return 0;
}

int rtems_esp_netif_stop( void )
{
  int rv = 0;

  if ( !rtems_esp_netif_attached ) {
    return 0;
  }

  if ( rtems_esp_netif_driver_ops->detach != NULL ) {
    rv = rtems_esp_netif_driver_ops->detach();
  }

  rtems_esp_netif_attached = false;
  rtems_esp_netif_core_call( rtems_esp_netif_do_link_down, "link down" );
  rtems_esp_netif_core_call( rtems_esp_netif_do_down, "interface down" );

  return rv;
}

/*
 * The WiFi events that mean something to a netif.  The event path already
 * works -- src/rtems_esp_event.c carries it and the wifi-init example prints
 * these -- so this is four cases and no new mechanism.
 *
 * STA_START rather than netif creation is where the receive path is installed:
 * esp_wifi_internal_reg_rxcb() is per interface, the header does not say
 * whether the interface has to exist first, and STA_START is the first moment
 * at which it certainly does.  Untested -- see docs/step8-netif.md.
 */
static void rtems_esp_netif_on_wifi_event(
  void       *arg,
  const char *base,
  int32_t     id,
  void       *data
)
{
  (void) arg;
  (void) base;
  (void) data;

  switch ( id ) {
    case WIFI_EVENT_STA_START:
      (void) rtems_esp_netif_start();
      break;

    case WIFI_EVENT_STA_STOP:
      (void) rtems_esp_netif_stop();
      break;

    case WIFI_EVENT_STA_CONNECTED:
      ++rtems_esp_netif_counters.link_up;
      rtems_esp_netif_core_call( rtems_esp_netif_do_link_up, "link up" );
      break;

    case WIFI_EVENT_STA_DISCONNECTED:
      ++rtems_esp_netif_counters.link_down;
      rtems_esp_netif_core_call( rtems_esp_netif_do_link_down, "link down" );
      break;

    default:
      break;
  }
}

struct netif *rtems_esp_netif_add(
  struct netif                 *netif,
  const rtems_esp_netif_driver *driver,
  const ip4_addr_t             *ipaddr,
  const ip4_addr_t             *netmask,
  const ip4_addr_t             *gateway
)
{
  rtems_status_code sc;
  struct netif     *added;

  if (
    driver == NULL
      || driver->attach == NULL
      || driver->transmit == NULL
      || driver->get_mac == NULL
  ) {
    printk( "rtems-esp-netif: the driver is incomplete\n" );

    return NULL;
  }

  if ( rtems_esp_netif_added ) {
    printk( "rtems-esp-netif: there is already a station netif\n" );

    return NULL;
  }

  sc = rtems_semaphore_create(
    rtems_build_name( 'N', 'E', 'T', 'X' ),
    1,
    RTEMS_BINARY_SEMAPHORE | RTEMS_PRIORITY | RTEMS_INHERIT_PRIORITY,
    0,
    &rtems_esp_netif_staging_lock
  );

  if ( sc != RTEMS_SUCCESSFUL ) {
    printk( "rtems-esp-netif: no semaphore (%i)\n", (int) sc );

    return NULL;
  }

  rtems_esp_netif_driver_ops = driver;

  /*
   * Published before netif_add, because netif_add runs the init callback and
   * -- once the tcpip thread is running -- may announce the interface before
   * it returns.  Anything reached from there that looks the netif up must find
   * it already.
   */
  rtems_esp_netif = netif != NULL ? netif : &rtems_esp_netif_own;

  /*
   * tcpip_input, not netif_input.  The long comment on
   * rtems_esp_netif_input() says why; the short version is that the receive
   * callback runs on the WiFi task and the IP stack should not.
   */
  added = netif_add(
    rtems_esp_netif,
    ipaddr,
    netmask,
    gateway,
    NULL,
    rtems_esp_netif_init,
    tcpip_input
  );

  if ( added == NULL ) {
    printk( "rtems-esp-netif: netif_add failed\n" );
    rtems_semaphore_delete( rtems_esp_netif_staging_lock );
    rtems_esp_netif_driver_ops = NULL;
    rtems_esp_netif = NULL;

    return NULL;
  }

  rtems_esp_netif_added = true;

  /*
   * After netif_add, so that an event arriving during registration finds a
   * netif; before esp_wifi_start(), which is the caller's next move and which
   * is what posts STA_START.  A handler that is not there when the event is
   * posted never hears it -- the event path drops rather than queues for a
   * late subscriber.
   */
  if (
    esp_event_handler_register(
      WIFI_EVENT,
      ESP_EVENT_ANY_ID,
      rtems_esp_netif_on_wifi_event,
      NULL
    ) != ESP_OK
  ) {
    printk( "rtems-esp-netif: no event handler slot; link state will not "
            "follow the radio\n" );
  }

  return rtems_esp_netif;
}

struct netif *rtems_esp_netif_get( void )
{
  return rtems_esp_netif_added ? rtems_esp_netif : NULL;
}

const rtems_esp_netif_stats *rtems_esp_netif_get_stats( void )
{
  return &rtems_esp_netif_counters;
}

void rtems_esp_netif_print_stats( void )
{
  const rtems_esp_netif_stats *s = &rtems_esp_netif_counters;

  printk( "rx_frames             %u\n", (unsigned) s->rx_frames );
  printk( "rx_bytes              %u\n", (unsigned) s->rx_bytes );
  printk( "rx_dropped_no_pbuf    %u\n", (unsigned) s->rx_dropped_no_pbuf );
  printk( "rx_dropped_stack_busy %u\n", (unsigned) s->rx_dropped_stack_busy );
  printk( "rx_dropped_bad_frame  %u\n", (unsigned) s->rx_dropped_bad_frame );
  printk( "eb_taken              %u\n", (unsigned) s->eb_taken );
  printk( "eb_released           %u\n", (unsigned) s->eb_released );
  printk( "tx_frames             %u\n", (unsigned) s->tx_frames );
  printk( "tx_bytes              %u\n", (unsigned) s->tx_bytes );
  printk( "tx_linearised         %u\n", (unsigned) s->tx_linearised );
  printk( "tx_dropped_too_long   %u\n", (unsigned) s->tx_dropped_too_long );
  printk( "tx_dropped_driver     %u\n", (unsigned) s->tx_dropped_driver );
  printk( "link_up               %u\n", (unsigned) s->link_up );
  printk( "link_down             %u\n", (unsigned) s->link_down );
}

/* ------------------------------------------------------------------------ *
 *      the blob driver -- the only part a substitution replaces            *
 * ------------------------------------------------------------------------ */

/*
 * Three entry points out of libnet80211.a and libpp.a, with the signatures as
 * esp_private/wifi.h declares them:
 *
 *   int       esp_wifi_internal_tx(wifi_interface_t, void *, uint16_t);
 *   esp_err_t esp_wifi_internal_reg_rxcb(wifi_interface_t, wifi_rxcb_t);
 *   void      esp_wifi_internal_free_rx_buffer(void *);
 *
 * and wifi_rxcb_t is
 *
 *   esp_err_t (*)(void *buffer, uint16_t len, void *eb);
 *
 * Verified present with riscv-rtems7-nm: esp_wifi_internal_reg_rxcb and
 * esp_wifi_internal_tx in libnet80211.a (ieee80211_api.o and
 * ieee80211_output.o), esp_wifi_internal_free_rx_buffer in libpp.a
 * (if_hwctrl.o).
 */

/*
 * The trampoline.  It exists because wifi_rxcb_t carries no context pointer,
 * so this is where a per-interface static would have to be consulted if there
 * were more than one interface.
 *
 * It returns ESP_OK unconditionally, and that is not laziness: the libraries'
 * reaction to a non-OK return from the RX callback is undocumented, and the
 * netif has already dealt with the frame -- counted it, and given the handle
 * back -- by the time this returns, so there is no state left for the caller
 * to act on.  Everything a caller might want to know is in the counters.
 */
static esp_err_t rtems_esp_netif_blob_rxcb( void *buffer, uint16_t len, void *eb )
{
  rtems_esp_netif_input( buffer, len, eb );

  return ESP_OK;
}

static int rtems_esp_netif_blob_attach( void )
{
  return (int) esp_wifi_internal_reg_rxcb(
    WIFI_IF_STA,
    rtems_esp_netif_blob_rxcb
  );
}

static int rtems_esp_netif_blob_detach( void )
{
  /*
   * A null callback is how ESP-IDF unregisters, and the header's "we support
   * only one RX callback for each interface" is the whole of what it says
   * about the subject.  If this turns out not to be an unregister, the
   * consequence is a callback still installed after stop, which
   * rtems_esp_netif_input() survives: rtems_esp_netif_added is false, so the
   * frame is dropped and the handle is still returned.
   */
  return (int) esp_wifi_internal_reg_rxcb( WIFI_IF_STA, NULL );
}

static int rtems_esp_netif_blob_transmit( void *frame, uint16_t len )
{
  return esp_wifi_internal_tx( WIFI_IF_STA, frame, len );
}

static void rtems_esp_netif_blob_release_rx( void *eb )
{
  esp_wifi_internal_free_rx_buffer( eb );
}

static int rtems_esp_netif_blob_get_mac( uint8_t mac[ 6 ] )
{
  return (int) esp_wifi_get_mac( WIFI_IF_STA, mac );
}

const rtems_esp_netif_driver rtems_esp_netif_blob_driver = {
  .name       = "esp_wifi_internal",
  .attach     = rtems_esp_netif_blob_attach,
  .detach     = rtems_esp_netif_blob_detach,
  .transmit   = rtems_esp_netif_blob_transmit,
  .release_rx = rtems_esp_netif_blob_release_rx,
  .get_mac    = rtems_esp_netif_blob_get_mac
};

/*
 * rtems-lwip's hook, and the reason this file is on the link line at all.
 *
 * rtemslwip/esp32c3/netstart.c brings the stack up and then calls this weakly
 * declared function to get an interface.  Defining it strongly here is what
 * joins the two halves of the port: an application calls start_networking()
 * and gets a WiFi station without naming anything in this file.
 *
 * It lives below the comment band, with the blob driver, because it names
 * rtems_esp_netif_blob_driver.  A substitution -- an ESP32 Open MAC driver,
 * say -- replaces this function along with the table above it, and the half of
 * the file above the band is untouched.
 *
 * Three things about the arguments are worth stating, because each is a place
 * the rtems-lwip convention and a radio disagree.
 *
 * `ipaddr` and friends are ip_addr_t, the dual-stack union, because LWIP_IPV6
 * is 1 in this BSP's lwipopts.h.  netif_add() wants the ip4_addr_t inside it,
 * and reading that out of a union tagged IPv6 would silently configure
 * nonsense, so the type tag is checked rather than assumed.  NULL is allowed
 * and means "DHCP will supply it".
 *
 * `mac_address` is accepted and not used.  On the Zynq it is an input: the
 * driver programs the GEM with whatever the application chose.  A station's
 * MAC is not the application's to choose -- it comes from eFuse, the WiFi
 * libraries derive the interface addresses from it, and a netif claiming a
 * different one would ARP for addresses the radio never accepts.  So the
 * netif's hwaddr comes from the driver's get_mac, which is esp_wifi_get_mac,
 * and an application that wants to know it reads netif->hwaddr afterwards.
 *
 * The return is zero for success, which is the weak default's contract and the
 * opposite of the rest of rtems-lwip.
 */
int esp32c3_netif_add(
  struct netif  *net_interface,
  ip_addr_t     *ipaddr,
  ip_addr_t     *netmask,
  ip_addr_t     *gateway,
  unsigned char *mac_address
)
{
  struct netif *added;

  (void) mac_address;

  if ( !IP_IS_V4( ipaddr ) || !IP_IS_V4( netmask ) || !IP_IS_V4( gateway ) ) {
    printk( "rtems-esp-netif: the station takes an IPv4 address; IPv6 comes\n"
            "rtems-esp-netif: from SLAAC once the interface is up\n" );

    return 1;
  }

  added = rtems_esp_netif_add(
    net_interface,
    &rtems_esp_netif_blob_driver,
    ipaddr != NULL ? ip_2_ip4( ipaddr ) : NULL,
    netmask != NULL ? ip_2_ip4( netmask ) : NULL,
    gateway != NULL ? ip_2_ip4( gateway ) : NULL
  );

  if ( added == NULL ) {
    return 1;
  }

  /*
   * The station is the only interface this part has, so it is the default
   * route whether or not it is up yet; without this every connect() outside
   * the loopback network fails with ERR_RTE.  netif_set_up() is deliberately
   * not called here -- that waits for WIFI_EVENT_STA_START, which is the
   * moment the receive path can be installed.
   */
  netif_set_default( added );

  return 0;
}
