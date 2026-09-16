/*
 * One image with lwIP, the netif and the WiFi libraries in it.
 *
 * Each of those three has worked on its own and no two of them had ever been
 * linked together: liblwip.a reaches socket() with no interface,
 * src/rtems_esp_netif.c had never executed a line, and examples/wifi-init
 * starts the radio with no stack above it.  This is the join, and the checks
 * below are the ones that become possible once it exists.
 *
 * WHAT THIS CANNOT SHOW, AND DOES NOT PRETEND TO
 *
 * No frame moves.  QEMU's esp32c3 model has no WiFi MAC: it raises no
 * interrupt and injects nothing, so the receive path cannot run at all and the
 * RX buffer-ownership contract -- the part docs/step8-netif.md calls the main
 * risk -- stays exactly as untested as it was.  There is deliberately no check
 * here that would appear to test it; rx_frames is printed instead, and a zero
 * there is the statement that nothing was proved about receive.
 *
 * Association needs an access point, so the link never comes up either and
 * every transmit is expected to be refused by the libraries.  What a transmit
 * attempt does show is that lwIP's output path reaches esp_wifi_internal_tx()
 * and comes back, which is a fault that did not happen rather than a packet
 * that did.
 *
 * THE ONE CHECK WORTH READING TWICE
 *
 * "the interface came up" is not cosmetic here.  netif_set_up() is only
 * reached from rtems_esp_netif_start(), which the WIFI_EVENT_STA_START handler
 * calls, and only after the driver's attach() -- esp_wifi_internal_reg_rxcb()
 * -- has returned success.  So that one boolean says the event path ran on its
 * own task, the netif's handler was registered in time to hear the event, the
 * libraries accepted the receive callback, and tcpip_callback() got the work
 * onto the tcpip thread.  It cannot pass without all four.
 *
 * It still does not say a frame will arrive.  Accepted is not delivered.
 */

#include <rtems-esp/newlib-compat.h>

#include <rtems-esp/netif.h>

#include <esp_wifi.h>
/* For the one direct call to esp_wifi_internal_tx() at the end, which asks the
 * libraries what they think of a transmit rather than only counting that they
 * refused one. */
#include <esp_private/wifi.h>
/*
 * wifi_promiscuous_pkt_t lives here rather than in esp_wifi_types.h.  It is
 * the per-chip half of the types header, and it has to be: wifi_pkt_rx_ctrl_t
 * is 48 bytes on the C3 against 28 on the original ESP32, so a payload offset
 * taken from the wrong one lands in the middle of the metadata.
 */
#include <esp_wifi_types_native.h>

/* start_networking(), from the BSP's installed rtems-lwip headers. */
#include <netstart.h>

#include <lwip/dhcp.h>
#include <lwip/netif.h>
#include <lwip/netifapi.h>
#include <lwip/pbuf.h>
#include <lwip/sockets.h>

#include <rtems.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check( const char *what, bool ok )
{
  printf( "%-52s %s\n", what, ok ? "ok" : "FAIL" );

  if ( !ok ) {
    ++failures;
  }
}

/*
 * QEMU's user-mode NAT addresses, which is the convention tests/zynq-lwip
 * follows.  Nothing routes here -- there is no NIC behind this netif -- but a
 * static address means no check below depends on a DHCP server answering, and
 * it keeps the two lwIP examples in this tree describing the same network.
 */
/*
 * How long to wait for a DHCP lease once associated.  Generous: a sleeping
 * access point can take several seconds to answer a discover, and the cost of
 * being wrong here is a spurious failure on a working network.
 */
#define DHCP_WAIT_SECONDS 20

/* Where WIFI_NET_UDP_PROBE shouts, and how many times. */
#ifndef WIFI_NET_UDP_PROBE_PORT
#define WIFI_NET_UDP_PROBE_PORT 47777
#endif
#ifndef WIFI_NET_UDP_PROBE_COUNT
#define WIFI_NET_UDP_PROBE_COUNT 20
#endif

#define STA_IP_A 10
#define STA_IP_B 0
#define STA_IP_C 2
#define STA_IP_D 15

static struct netif net_interface;

#ifdef WIFI_NET_PROMISC_PROBE
static volatile unsigned promisc_frames;
static volatile unsigned promisc_data;

/*
 * Counts everything the radio hears while associated.
 *
 * It answers one question that the netif counters cannot: rx_frames 0 with
 * every rx_dropped_* also 0 means our callback was never called, and that is
 * equally what "the radio hears nothing" and "the radio hears plenty and the
 * libraries keep all of it" look like.  Promiscuous mode sits below the
 * filtering and the decryption, so a non-zero count here puts the fault above
 * the MAC and a zero one puts it below.
 *
 * The data subtype is counted separately because that is the interesting one.
 * Beacons prove the receive chain works at all -- the scan already did -- but
 * only data frames can become a DHCP offer.
 */
static volatile unsigned promisc_to_us;
static volatile unsigned promisc_data_to_us;

/*
 * addr1 of an 802.11 header is the receiver address, and it is the only field
 * that answers the question that matters here: is the access point sending
 * anything to this station at all?
 *
 * "No frames reach the netif" has two causes that look identical from the
 * netif side and want opposite fixes.  Either the access point is not
 * transmitting to us -- our own transmits are being accepted by the driver and
 * going nowhere, so nothing is ever answered -- or it is transmitting and the
 * libraries are keeping every frame, which would point at decryption.
 * Counting by receiver address separates them.
 *
 * The header starts at payload: frame control (2), duration (2), then addr1.
 */
static void promisc_cb( void *buf, wifi_promiscuous_pkt_type_t type )
{
  const wifi_promiscuous_pkt_t *pkt = buf;
  bool                          to_us;

  ++promisc_frames;

  if ( pkt == NULL ) {
    return;
  }

  to_us = memcmp( pkt->payload + 4, net_interface.hwaddr, 6 ) == 0;

  if ( to_us ) {
    ++promisc_to_us;
  }

  if ( type == WIFI_PKT_DATA ) {
    ++promisc_data;

    if ( to_us ) {
      ++promisc_data_to_us;
    }
  }
}
#endif

/*
 * start_networking() takes one and the ESP32-C3 ignores it: a station's MAC
 * comes from eFuse and the WiFi libraries derive the interface addresses from
 * it.  Passed anyway, because the signature is rtems-lwip's, and checked
 * against what the netif really reports below.
 */
static unsigned char mac_ignored[ 6 ] = { 0x02, 0x52, 0x54, 0x00, 0x12, 0x34 };

/*
 * The application's own view of the events, printed so that the log says which
 * of them actually reached a handler.  The netif registers its own separately;
 * this one exists because "the interface came up" alone would not distinguish
 * an event that never fired from a handler that ran and failed.
 */
/*
 * The network to associate with.
 *
 * Deliberately a placeholder rather than anything real.  Nothing in QEMU
 * answers a probe request, so what this exercises is the path from
 * esp_wifi_set_config() through esp_wifi_connect() into the scan, not an
 * association -- and on hardware it should be overridden at build time rather
 * than edited here, so that credentials never reach a commit.
 */
#ifndef WIFI_NET_SSID
#define WIFI_NET_SSID "rtems-test-network"
#endif

/*
 * Empty, and that is a decision rather than an oversight.
 *
 * libnet80211's scan_parse_beacon tests the configured password against the
 * candidate's privacy bit BEFORE any authmode threshold is consulted: with a
 * non-empty password it refuses an open access point outright, logging "Open
 * AP, but we want an encrypted AP, ignore".  So a station configured with a
 * password can only ever associate with an encrypted network, and associating
 * with the simulated AP in QEMU would then need the WPA2 four-way handshake --
 * PBKDF2, PTK derivation, MIC, GTK unwrap -- and CCMP after it.
 *
 * An empty password takes the open path instead, which is what lets a frame
 * reach the netif at all in emulation.  On hardware this must be overridden:
 * pass -DWIFI_NET_PASSWORD=\"...\" at build time, which is also how the SSID
 * is meant to be supplied.
 */
#ifndef WIFI_NET_PASSWORD
#define WIFI_NET_PASSWORD ""
#endif

static volatile int sta_start_seen;
static volatile int sta_connected_seen;
static volatile int sta_disconnected_seen;

static void on_wifi_event(
  void       *arg,
  const char *base,
  int32_t     id,
  void       *data
)
{
  (void) arg;

  printf( "       event: %s id %i\n", base, (int) id );

  if ( base != WIFI_EVENT ) {
    return;
  }

  /*
   * Counted rather than just printed, because the interesting assertions
   * below are about which of these arrived.  A disconnect is the expected
   * outcome here and is not a failure -- see the check that reads it.
   */
  switch ( id ) {
    case WIFI_EVENT_STA_START:        ++sta_start_seen;        break;
    case WIFI_EVENT_STA_CONNECTED:    ++sta_connected_seen;    break;
    case WIFI_EVENT_STA_DISCONNECTED:
      ++sta_disconnected_seen;

      /*
       * The reason, not just the count.  "Disconnected" on its own does not
       * say whether the access point was never seen, refused the credentials,
       * or dropped the association later, and those want three different
       * things done about them.  The common ones on a first attempt are
       * WIFI_REASON_NO_AP_FOUND (201), which on this chip usually means the
       * network is 5GHz and the C3 is 2.4GHz only, and
       * WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT (15), which means the passphrase
       * was wrong.
       */
      if ( data != NULL ) {
        const wifi_event_sta_disconnected_t *d = data;

        printf( "       disconnected: reason %i, rssi %i, ssid '%.*s'\n",
                (int) d->reason, (int) d->rssi,
                (int) d->ssid_len, (const char *) d->ssid );
      }

      break;
    default: break;
  }
}

static void print_mac( const char *what, const uint8_t mac[ 6 ] )
{
  printf( "       %s %02x:%02x:%02x:%02x:%02x:%02x\n", what,
          mac[ 0 ], mac[ 1 ], mac[ 2 ], mac[ 3 ], mac[ 4 ], mac[ 5 ] );
}

/*
 * Fill in something shaped like an Ethernet frame: broadcast destination, the
 * station's own address as source, and a length that is a legal minimum frame.
 * The contents do not matter -- nothing will parse them -- but a frame the
 * libraries would reject on inspection would confuse a failure for a refusal.
 */
static void fill_frame( uint8_t *p, uint16_t len, const uint8_t src[ 6 ] )
{
  memset( p, 0, len );
  memset( p, 0xff, 6 );
  memcpy( p + 6, src, 6 );
  p[ 12 ] = 0x08;
  p[ 13 ] = 0x00;
}

/*
 * One transmit attempt through the netif's own linkoutput, which is where lwIP
 * would enter it.
 *
 * Called straight from Init rather than through a socket, because a socket
 * send needs ARP to resolve, ARP needs a reply, and a reply needs a frame to
 * arrive -- which is the one thing that cannot happen here.  Going in at
 * linkoutput is the only way to reach esp_wifi_internal_tx() in emulation at
 * all.
 *
 * Returns the err_t so the caller can report it.  A refusal is the expected
 * result: the station has not associated, so the libraries have nowhere to put
 * the frame.  What is being checked is that the call returned.
 */
static err_t transmit_one( struct netif *netif, bool chained )
{
  struct pbuf *p;
  err_t        err;
  uint8_t      frame[ 60 ];

  fill_frame( frame, sizeof( frame ), netif->hwaddr );

  if ( !chained ) {
    p = pbuf_alloc( PBUF_RAW, sizeof( frame ), PBUF_RAM );

    if ( p == NULL ) {
      return ERR_MEM;
    }

    memcpy( p->payload, frame, sizeof( frame ) );
  } else {
    /*
     * A header pbuf followed by a data pbuf, which is the shape a TCP segment
     * arrives in and therefore the common case rather than the exception.  It
     * is the only way to reach the staging buffer and its lock.
     */
    struct pbuf *tail;

    p = pbuf_alloc( PBUF_RAW, 14, PBUF_RAM );
    tail = pbuf_alloc( PBUF_RAW, sizeof( frame ) - 14, PBUF_RAM );

    if ( p == NULL || tail == NULL ) {
      if ( p != NULL ) {
        pbuf_free( p );
      }

      if ( tail != NULL ) {
        pbuf_free( tail );
      }

      return ERR_MEM;
    }

    memcpy( p->payload, frame, 14 );
    memcpy( tail->payload, frame + 14, sizeof( frame ) - 14 );
    pbuf_cat( p, tail );
  }

  err = netif->linkoutput( netif, p );
  pbuf_free( p );

  return err;
}

static rtems_task Init( rtems_task_argument arg )
{
  wifi_init_config_t           config = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t                    rv;
  ip_addr_t                    ipaddr, netmask, gateway;
  uint8_t                      wifi_mac[ 6 ];
  const rtems_esp_netif_stats *stats;
  uint32_t                     tx_before;
  err_t                        err_single, err_chained, err;
  int                          sock;

  (void) arg;

  /*
   * Fetched here rather than beside its first use further down.  It is a
   * pointer to a static counter block in the port, so it is valid before
   * anything is initialised -- and the "goto done" on an esp_wifi_init()
   * failure jumps straight past the assignment that used to be the only one,
   * into a block that dereferences it.  That path has never been taken, which
   * is the only reason it has not crashed.
   */
  stats = rtems_esp_netif_get_stats();

  printf( "\n*** ESP32-C3 WIFI + LWIP TEST ***\n" );

  rv = esp_event_handler_register( WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   on_wifi_event, NULL );
  check( "esp_event_handler_register", rv == ESP_OK );

  rv = esp_wifi_init( &config );
  check( "esp_wifi_init", rv == ESP_OK );

  if ( rv != ESP_OK ) {
    goto done;
  }

  rv = esp_wifi_set_mode( WIFI_MODE_STA );
  check( "esp_wifi_set_mode(STA)", rv == ESP_OK );

  /*
   * IP_ADDR4 rather than IP4_ADDR: LWIP_IPV6 is 1 in this BSP's lwipopts.h, so
   * ip_addr_t is the dual-stack union and the type tag has to be set as well
   * as the bytes.  esp32c3_netif_add() checks that tag rather than trusting
   * it.
   */
  IP_ADDR4( &ipaddr, STA_IP_A, STA_IP_B, STA_IP_C, STA_IP_D );
  IP_ADDR4( &netmask, 255, 255, 255, 0 );
  IP_ADDR4( &gateway, STA_IP_A, STA_IP_B, STA_IP_C, 2 );

  /*
   * Between set_mode and start, and both halves of that matter.  The netif
   * asks the driver for a MAC from inside netif_add, which needs the
   * libraries initialised; and it registers the WiFi event handler that hears
   * STA_START, which esp_wifi_start() is about to post.
   */
  printf( "calling start_networking...\n" );
  check( "start_networking",
         start_networking( &net_interface, &ipaddr, &netmask, &gateway,
                           mac_ignored ) == 0 );

  /*
   * netif_add() accepted it.  netif_default is the evidence that
   * esp32c3_netif_add() ran and won over liblwip's weak default -- the weak
   * one adds no interface at all, so this cannot pass without the override.
   */
  check( "the netif is registered and is the default route",
         netif_default == &net_interface );
  check( "and it is the one the port reports",
         rtems_esp_netif_get() == &net_interface );
  check( "named wl0",
         net_interface.name[ 0 ] == 'w' && net_interface.name[ 1 ] == 'l' );
  check( "with an Ethernet MTU", net_interface.mtu == 1500 );
  check( "and the address it was given",
         ip4_addr_get_u32( netif_ip4_addr( &net_interface ) )
           == ip4_addr_get_u32( ip_2_ip4( &ipaddr ) ) );

  /*
   * The MAC the netif reports against the MAC the libraries report.  This is
   * the first time read_mac has been checked against anything: QEMU's eFuse
   * reads as zeros, so the value is printed as well as compared -- an all-zero
   * address is a legitimate outcome of this emulation and is not the same
   * claim as "the MAC is right".
   */
  memset( wifi_mac, 0, sizeof( wifi_mac ) );
  rv = esp_wifi_get_mac( WIFI_IF_STA, wifi_mac );
  check( "esp_wifi_get_mac", rv == ESP_OK );
  print_mac( "esp_wifi_get_mac ", wifi_mac );
  print_mac( "netif->hwaddr    ", net_interface.hwaddr );
  check( "the netif reports the MAC the libraries do",
         memcmp( net_interface.hwaddr, wifi_mac, 6 ) == 0 );
  check( "which is not the one the application passed",
         memcmp( net_interface.hwaddr, mac_ignored, 6 ) != 0 );

  /*
   * Printed rather than asserted, because the weakness is in the value and not
   * in the code: two all-zero addresses agree for free.  On hardware this line
   * does not appear and the check above means what it says.
   */
  {
    static const uint8_t zero[ 6 ] = { 0 };

    if ( memcmp( wifi_mac, zero, 6 ) == 0 ) {
      printf(
        "       both are all-zero, which is what QEMU's eFuse reads as.\n"
        "       So read_mac ran and the netif took its answer; whether that\n"
        "       answer is right needs a part with a real eFuse.\n"
      );
    }
  }

  check( "the interface starts link-down, as a radio should",
         !netif_is_link_up( &net_interface ) );

  /*
   * The credentials have to be set before esp_wifi_start(), not after.
   * esp_wifi_connect() reads the configuration the station was started with;
   * setting it afterwards is accepted and then ignored until the next start,
   * which presents as a connect that never attempts anything.
   */
  {
    wifi_config_t sta = { 0 };

    strncpy( (char *) sta.sta.ssid, WIFI_NET_SSID, sizeof( sta.sta.ssid ) - 1 );
    strncpy( (char *) sta.sta.password, WIFI_NET_PASSWORD,
             sizeof( sta.sta.password ) - 1 );

    rv = esp_wifi_set_config( WIFI_IF_STA, &sta );
    check( "esp_wifi_set_config(STA)", rv == ESP_OK );
  }

  printf( "calling esp_wifi_start...\n" );
  rv = esp_wifi_start();
  check( "esp_wifi_start", rv == ESP_OK );

  /*
   * The event task runs at priority 100 and Init at 1, so nothing dispatched
   * from it can have run yet.  Blocking here is what lets it, and the tcpip
   * thread behind it, reach the checks below; without this every one of them
   * would report a failure that is only this task's priority.
   */
  rtems_task_wake_after( 2 * rtems_clock_get_ticks_per_second() );

  /*
   * See the header comment: this cannot pass unless STA_START was dispatched,
   * esp_wifi_internal_reg_rxcb() returned success and tcpip_callback() ran.
   */
  check( "the receive path was accepted and the interface came up",
         netif_is_up( &net_interface ) );

  check( "STA_START was delivered to the handler", sta_start_seen > 0 );

  /*
   * Associate.
   *
   * Nothing in QEMU answers a probe request, so this is not expected to
   * connect and the check below is written for that: what it asserts is that
   * the call was accepted and that the libraries then reported a *result*.
   *
   * That is worth having even without an access point.  esp_wifi_connect()
   * drives the scan, the supplicant and the connection state machine, and a
   * disconnect event coming back means all three ran and reached a verdict
   * rather than hanging -- which is the failure mode a half-wired OS adapter
   * produces.
   *
   * It is also the prerequisite for a frame ever reaching the netif.  QEMU's
   * MAC model injects frames today and libnet80211 drops them in sta_input,
   * correctly, because without a connection attempt there is no BSS a data
   * frame could belong to.
   */
  /*
   * Scan before connecting, and print what is out there.
   *
   * A disconnect with WIFI_REASON_NO_AP_FOUND has two very different causes
   * and the reason code cannot tell them apart: either the receive path does
   * not work and the station hears nothing at all, or it works and the
   * network asked for is genuinely not on the air.  A list of everything
   * heard separates them in one run.
   *
   * It is also the first thing in this example that requires receiving:
   * everything above it is configuration and transmit.  Every beacon and
   * probe response in the list came off the antenna, through the PHY, through
   * the MAC and up into libnet80211, so a non-empty list is the receive path
   * working end to end.
   *
   * Blocking, so the records are ready when the call returns.  The C3 is a
   * 2.4GHz radio, so a 5GHz-only network cannot appear here however correct
   * everything else is.
   */
  {
    uint16_t found = 0;

    printf( "scanning...\n" );
    rv = esp_wifi_scan_start( NULL, true );
    printf( "esp_wifi_scan_start returned %i\n", (int) rv );

    if ( rv == ESP_OK && esp_wifi_scan_get_ap_num( &found ) == ESP_OK ) {
      wifi_ap_record_t *records = calloc( found ? found : 1,
                                          sizeof( *records ) );

      printf( "       %u access point(s) heard\n", (unsigned) found );

      if ( records != NULL && found > 0 ) {
        uint16_t n = found;

        if ( esp_wifi_scan_get_ap_records( &n, records ) == ESP_OK ) {
          uint16_t i;

          for ( i = 0; i < n; ++i ) {
            printf( "       ch %2u  rssi %4i  auth %u  '%s'\n",
                    (unsigned) records[ i ].primary,
                    (int) records[ i ].rssi,
                    (unsigned) records[ i ].authmode,
                    (const char *) records[ i ].ssid );
          }
        }
      }

      free( records );
    }

    /*
     * Not a failure on its own.  In QEMU it is zero unless the MAC model's
     * simulated access point is enabled, and this example has to pass there
     * too -- what it would mean on hardware is said above, in the log rather
     * than in an assertion.
     */
    check( "the scan was accepted", rv == ESP_OK );
  }

  /*
   * Power save off, before associating.
   *
   * ESP-IDF defaults a station to WIFI_PS_MIN_MODEM: the radio sleeps between
   * DTIMs and wakes in time for the next beacon.  That needs the sleep side of
   * the OS adapter to work, and on this port it does not -- see the named
   * failures in rtems_wifi_os_adapter.c.  What that looks like from outside is
   * an association that succeeds and then goes deaf:
   * WIFI_EVENT_STA_BEACON_TIMEOUT arrives a few seconds later, DHCP discovers
   * go out and nothing ever comes back, and the counters show transmit working
   * and rx_frames stuck at 0.
   *
   * Has to be after esp_wifi_start(); ESP-IDF rejects it before.
   */
  rv = esp_wifi_set_ps( WIFI_PS_NONE );
  printf( "esp_wifi_set_ps(WIFI_PS_NONE) returned %i\n", (int) rv );
  check( "power save could be turned off", rv == ESP_OK );

  printf( "calling esp_wifi_connect to %s...\n", WIFI_NET_SSID );
  rv = esp_wifi_connect();
  printf( "esp_wifi_connect returned %i\n", (int) rv );
  check( "esp_wifi_connect was accepted", rv == ESP_OK );

  /* Long enough for the scan to give up and post a result. */
  rtems_task_wake_after( 5 * rtems_clock_get_ticks_per_second() );

  printf( "       STA_START %d  CONNECTED %d  DISCONNECTED %d\n",
          sta_start_seen, sta_connected_seen, sta_disconnected_seen );

  /*
   * Either outcome is a pass.  Connected would mean something answered, which
   * nothing in QEMU does; disconnected means the stack ran to a conclusion.
   * Neither is a hang, and a hang is what this is really testing for.
   */
  check( "the connection attempt reached a verdict",
         sta_connected_seen > 0 || sta_disconnected_seen > 0 );

  /*
   * What was actually negotiated, when there is an association to ask about.
   *
   * esp_wifi_sta_get_ap_info() reports the ciphers the station and the access
   * point agreed on, which is worth having in the log next to a run where
   * transmit is accepted by the driver and nothing arrives: a pairwise cipher
   * of WIFI_CIPHER_TYPE_CCMP means the RSN negotiation completed and the data
   * path is expected to be encrypted, and it narrows the remaining question to
   * whether the key reached the hardware.
   */
  if ( sta_connected_seen > 0 ) {
    wifi_ap_record_t ap;

    if ( esp_wifi_sta_get_ap_info( &ap ) == ESP_OK ) {
      printf( "       associated to '%s' ch %u rssi %i\n",
              (const char *) ap.ssid, (unsigned) ap.primary, (int) ap.rssi );
      printf( "       authmode %u  pairwise cipher %u  group cipher %u\n",
              (unsigned) ap.authmode,
              (unsigned) ap.pairwise_cipher,
              (unsigned) ap.group_cipher );
    } else {
      printf( "       esp_wifi_sta_get_ap_info failed\n" );
    }
  }

  /*
   * DHCP, but only if the station actually associated.
   *
   * Gated on the event rather than on a build-time flag, because the event is
   * the condition that makes the difference.  A station that associated is on
   * somebody else's network and the address compiled in above is almost
   * certainly wrong for it; a station that did not has nothing to ask.  QEMU
   * has no DHCP server and never reaches WIFI_EVENT_STA_CONNECTED, so it keeps
   * the static address and the behaviour it had before, and the branch below
   * is hardware-only without needing to say so.
   *
   * netifapi_dhcp_start() rather than dhcp_start().  dhcp_start() is core work
   * -- it rewrites the netif's addresses and arms timeouts -- and this runs on
   * Init, not the tcpip thread.  The netifapi wrapper does the handover.
   *
   * This is also what finally exercises receiving for real.  The scan proved
   * beacons arrive, but a beacon is consumed inside libnet80211 and never
   * reaches this layer; a DHCP offer is addressed to this station and comes up
   * through esp32c3_netif_input(), which is the first frame to cross the eb
   * boundary on hardware and the first time the ownership contract below is
   * tested by anything other than QEMU.
   */
  if ( sta_connected_seen > 0 ) {
    unsigned waited;

    printf( "associated; starting DHCP...\n" );

    err = netifapi_dhcp_start( &net_interface );
    printf( "netifapi_dhcp_start returned %i\n", (int) err );
    check( "DHCP started", err == ERR_OK );

    /*
     * Polled rather than waited on.  A netif status callback would need to
     * hand the result back across threads for no gain here: the run has
     * nothing else to do until an address arrives, and a poll makes the
     * timeout explicit.
     */
    for ( waited = 0; waited < DHCP_WAIT_SECONDS; ++waited ) {
      if ( dhcp_supplied_address( &net_interface ) ) {
        break;
      }

      rtems_task_wake_after( rtems_clock_get_ticks_per_second() );
    }

    /*
     * A real failure, not a "did not happen".  Reaching here means the station
     * is associated to an access point, so a network that does not answer a
     * discover in this long is worth reporting as broken rather than shrugged
     * at -- and it is the only way this example can tell "associated" from
     * "usable".
     */
    check( "DHCP bound an address", dhcp_supplied_address( &net_interface ) );

    /*
     * One call per line: ip4addr_ntoa() returns a pointer to a single static
     * buffer, so two of them in one printf both show the second address.
     */
    printf( "       address %s\n",
            ip4addr_ntoa( netif_ip4_addr( &net_interface ) ) );
    printf( "       netmask %s\n",
            ip4addr_ntoa( netif_ip4_netmask( &net_interface ) ) );
    printf( "       gateway %s\n",
            ip4addr_ntoa( netif_ip4_gw( &net_interface ) ) );

#ifdef WIFI_NET_UDP_PROBE
    /*
     * Shout onto the LAN, so another host can say whether we are audible.
     *
     * Everything measured so far is from this station's own point of view, and
     * none of it separates the two remaining possibilities: either our data
     * frames never reach the access point -- accepted by the driver, encrypted
     * with a key the AP does not share, and silently discarded -- or they do
     * and the replies are being kept somewhere above the MAC.  A second
     * machine on the same network settles it in one run.  If the datagrams
     * arrive there, transmit works end to end and the fault is receive; if
     * they do not, it is transmit, and the DHCP silence is a consequence
     * rather than the problem.
     *
     * A chosen port rather than DHCP's own: port 68 needs privileges on the
     * listening side, and the point is a test anyone can run.
     *
     * The source address is wrong for that network -- it is the static one
     * compiled in above, since DHCP did not answer -- and that does not
     * matter.  Delivery here is layer 2: the access point bridges a broadcast
     * frame to the LAN whatever the IP header claims.
     */
    {
      int s = socket( AF_INET, SOCK_DGRAM, 0 );

      if ( s >= 0 ) {
        struct sockaddr_in to;
        int                on = 1;
        int                i;

        setsockopt( s, SOL_SOCKET, SO_BROADCAST, &on, sizeof( on ) );

        memset( &to, 0, sizeof( to ) );
        to.sin_family      = AF_INET;
        to.sin_port        = htons( WIFI_NET_UDP_PROBE_PORT );
        to.sin_addr.s_addr = htonl( INADDR_BROADCAST );

        printf( "sending %d UDP broadcasts to port %d...\n",
                WIFI_NET_UDP_PROBE_COUNT, WIFI_NET_UDP_PROBE_PORT );

        for ( i = 0; i < WIFI_NET_UDP_PROBE_COUNT; ++i ) {
          char msg[ 64 ];
          int  n = snprintf( msg, sizeof( msg ),
                             "rtems-esp32c3 %02x:%02x:%02x:%02x:%02x:%02x #%d",
                             net_interface.hwaddr[ 0 ], net_interface.hwaddr[ 1 ],
                             net_interface.hwaddr[ 2 ], net_interface.hwaddr[ 3 ],
                             net_interface.hwaddr[ 4 ], net_interface.hwaddr[ 5 ],
                             i );

          if ( sendto( s, msg, (size_t) n, 0,
                       (struct sockaddr *) &to, sizeof( to ) ) < 0 ) {
            printf( "       sendto %d failed\n", i );
          }

          rtems_task_wake_after( rtems_clock_get_ticks_per_second() / 2 );
        }

        close( s );
        printf( "       sent; tx_frames now %u\n",
                (unsigned) stats->tx_frames );
      }
    }
#endif

#ifdef WIFI_NET_PROMISC_PROBE
    if ( stats->rx_frames == 0 ) {
      printf( "no frames reached the netif; counting what the radio hears\n" );

      if ( esp_wifi_set_promiscuous_rx_cb( promisc_cb ) == ESP_OK &&
           esp_wifi_set_promiscuous( true ) == ESP_OK ) {
        rtems_task_wake_after( 5 * rtems_clock_get_ticks_per_second() );
        ( void ) esp_wifi_set_promiscuous( false );
      }

      printf( "       promiscuous heard %u frames, %u of them data\n",
              promisc_frames, promisc_data );
      printf( "       addressed to this station: %u frames, %u of them data\n",
              promisc_to_us, promisc_data_to_us );
    }
#endif
  }

  /*
   * Transmit.  Both shapes, because the chained one is the only path through
   * the staging buffer.  Refusal is expected and is not counted as a failure:
   * the station has not associated with anything.
   */
  tx_before = stats->tx_frames + stats->tx_dropped_driver;

  err_single = transmit_one( &net_interface, false );
  err_chained = transmit_one( &net_interface, true );

  printf( "       linkoutput returned %i for a single pbuf, %i for a chain\n",
          (int) err_single, (int) err_chained );

  check( "two transmits reached the driver and returned",
         ( stats->tx_frames + stats->tx_dropped_driver ) == tx_before + 2 );
  check( "the chained one was linearised", stats->tx_linearised == 1 );
  check( "neither was rejected for length", stats->tx_dropped_too_long == 0 );

  /*
   * The netif counts a driver refusal rather than printing it, deliberately:
   * a message per dropped frame turns a slow association into a console flood
   * that hides whatever came next.  So the libraries are asked once, directly,
   * because "the transmit reached esp_wifi_internal_tx" is worth more when the
   * log says what it answered.  An error code from inside libnet80211.a is
   * evidence the call arrived there; a zero from a stub would not be.
   */
  {
    uint8_t frame[ 60 ];
    int     answer;

    fill_frame( frame, sizeof( frame ), net_interface.hwaddr );
    answer = (int) esp_wifi_internal_tx( WIFI_IF_STA, frame,
                                         (uint16_t) sizeof( frame ) );

    /*
     * In hex as well, because that is how esp_wifi.h writes these:
     * ESP_ERR_WIFI_BASE is 0x3000 and the low digits name the reason.
     */
    printf( "       esp_wifi_internal_tx answered %i (0x%04x) directly\n",
            answer, (unsigned) answer );
  }

  /*
   * The stack above the netif still works.  Not a network test -- nothing can
   * be reached -- but a socket that cannot be created would mean lwIP was
   * mis-sized rather than merely unable to route.
   */
  sock = socket( AF_INET, SOCK_STREAM, 0 );
  check( "socket()", sock >= 0 );

  if ( sock >= 0 ) {
    close( sock );
  }

done:
  printf( "\nnetif counters:\n" );
  rtems_esp_netif_print_stats();

  /*
   * Said in words as well as in the zero above, because a reader who sees
   * eb_taken == eb_released might otherwise take it for a result.  It holds
   * vacuously: no frame reached this layer, so neither counter ever moved.
   *
   * This explanation has now gone stale twice, which is a warning in itself:
   * first when the MAC model learned to inject frames, and again when this
   * example started calling esp_wifi_connect().  The number has been 0
   * throughout and the reason has changed underneath it both times.  Check it
   * against what the run actually does before trusting it.
   */
  /*
   * Derived from the counters, not written out.
   *
   * This paragraph was hand-written three times and was wrong twice: once when
   * the MAC model learned to inject frames, and again when this example
   * started calling esp_wifi_connect().  Each time the number stayed accurate
   * and the sentence beside it rotted, which is worse than no sentence at all
   * -- a reader checks the prose, believes it, and stops looking.
   *
   * So it now reports what the run actually did.  The point being made is the
   * same either way: eb_taken == eb_released is trivially true when both are
   * zero, and only means something when frames have gone through.
   */
  if ( stats->rx_frames == 0 ) {
    printf(
      "\nrx_frames is 0, so eb_taken == eb_released above is true without\n"
      "having tested anything -- the receive path did not execute.  For it to\n"
      "run, the station has to associate: QEMU's MAC model needs its simulated\n"
      "access point enabled, and on hardware there has to be a real one.\n"
    );
  } else {
    printf(
      "\nrx_frames is %u, so eb_taken == eb_released is a real result: the\n"
      "receive path ran and every eb handle the driver lent us was given\n"
      "back.  That contract is the one thing here that cannot be checked by\n"
      "reading the code -- leaking a handle is not an error, it is a radio\n"
      "that goes quiet after the RX pool is exhausted.\n",
      (unsigned) stats->rx_frames
    );
  }

  /*
   * Asserted, not just narrated.  Every paragraph above has described this
   * contract and nothing has ever checked it, which is the failure mode #98
   * is about: a check that reads as a result and is not one.
   *
   * It holds vacuously while rx_frames is 0 -- which is why the explanation
   * above distinguishes the two cases -- but it is cheap and it is exactly the
   * assertion that has to be in place before the first frame arrives, not
   * after.  Leaking an eb handle is not an error at the time: it is a radio
   * that goes quiet once the RX pool is gone, a long way from here.
   */
  check( "every eb handle the driver lent us was given back",
         stats->eb_taken == stats->eb_released );

  printf( "\n%d failure(s)\n", failures );

  if ( failures == 0 ) {
    printf( "CI-MARKER wifi net ok\n" );
  }

  printf( "*** END OF ESP32-C3 WIFI + LWIP TEST ***\n" );

  exit( 0 );
}

#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
/* Sockets need a file descriptor table, which needs a filesystem. */
#define CONFIGURE_FILESYSTEM_IMFS
#define CONFIGURE_MAXIMUM_FILE_DESCRIPTORS 32
/* Init, the WiFi task, the event task, the tcpip thread and the interrupt
 * server, with room for what the libraries start on their own. */
#define CONFIGURE_MAXIMUM_TASKS 16
#define CONFIGURE_MAXIMUM_SEMAPHORES 64
#define CONFIGURE_MAXIMUM_MESSAGE_QUEUES 16
#define CONFIGURE_MAXIMUM_POSIX_KEYS 8
/* The WiFi libraries arm several ETS timers during init and start; 8 ran out
 * and the adapter said so.  Measured demand, not a guess. */
#define CONFIGURE_MAXIMUM_TIMERS 32
#define CONFIGURE_MAXIMUM_DRIVERS 8
#define CONFIGURE_UNIFIED_WORK_AREAS
#define CONFIGURE_INIT_TASK_STACK_SIZE ( 16 * 1024 )
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
