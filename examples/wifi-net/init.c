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

/* start_networking(), from the BSP's installed rtems-lwip headers. */
#include <netstart.h>

#include <lwip/netif.h>
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
#define STA_IP_A 10
#define STA_IP_B 0
#define STA_IP_C 2
#define STA_IP_D 15

static struct netif net_interface;

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
static void on_wifi_event(
  void       *arg,
  const char *base,
  int32_t     id,
  void       *data
)
{
  (void) arg;
  (void) data;

  printf( "       event: %s id %i\n", base, (int) id );
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
  err_t                        err_single, err_chained;
  int                          sock;

  (void) arg;

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

  /*
   * Transmit.  Both shapes, because the chained one is the only path through
   * the staging buffer.  Refusal is expected and is not counted as a failure:
   * the station has not associated with anything.
   */
  stats = rtems_esp_netif_get_stats();
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
   * The reason changed once QEMU's MAC model learned to inject frames, and the
   * old wording -- "QEMU has no WiFi MAC, so no frame can arrive" -- is no
   * longer true.  Frames do arrive now, and the MAC interrupt is delivered and
   * serviced; they are dropped one layer above this file.
   */
  printf(
    "\nrx_frames is 0, and the reason is no longer that nothing arrives.\n"
    "QEMU's MAC model injects frames and the interrupt is serviced -- they\n"
    "reach sta_input inside libnet80211 and are dropped there, correctly,\n"
    "because this image never calls esp_wifi_connect() so net80211 has no\n"
    "BSS a data frame could belong to.  So eb_taken == eb_released above is\n"
    "still true without having tested anything.\n"
  );

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
