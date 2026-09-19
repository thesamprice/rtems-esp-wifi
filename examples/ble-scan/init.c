/*
 * A BLE scan on RTEMS: bring the ESP32-C3 controller up and print every
 * advertiser it hears.
 *
 * The host side here is deliberately the smallest thing that can be called a
 * host: three HCI commands written straight into VHCI, and one event parser.
 * No Bluedroid, no NimBLE.  A scan needs Reset, LE Set Scan Parameters and LE
 * Set Scan Enable, and everything interesting comes back as one event type.
 */

#include "bt_sdkconfig.h"

#include <rtems.h>
#include <rtems/bspIo.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"

extern int rtems_esp_bt_controller_init( void *config );
extern int rtems_esp_bt_controller_enable( void );

extern bool API_vhci_host_check_send_available( void );
extern void API_vhci_host_send_packet( uint8_t *data, uint16_t len );
extern int  API_vhci_host_register_callback( const void *callback );

/* -------------------------------------------------------------------- */

#define H4_COMMAND 0x01
#define H4_EVENT   0x04

#define HCI_EV_CMD_COMPLETE 0x0e
#define HCI_EV_LE_META      0x3e
#define HCI_SUBEV_ADV_REPORT 0x02

static volatile bool     cmd_done;
static volatile uint16_t cmd_done_opcode;
static volatile uint8_t  cmd_done_status;
static volatile unsigned reports;
static volatile unsigned events;

/*
 * Advertisers already seen, so a busy room prints a list rather than a
 * scrolling repeat.  The controller's own duplicate filter is on as well;
 * this is belt and braces and also what lets the run report a count.
 */
#define MAX_SEEN 40
static uint8_t seen[ MAX_SEEN ][ 6 ];
static int     seen_count;

static bool remember( const uint8_t *addr )
{
  for ( int i = 0; i < seen_count; ++i ) {
    if ( memcmp( seen[ i ], addr, 6 ) == 0 ) {
      return false;
    }
  }

  if ( seen_count < MAX_SEEN ) {
    memcpy( seen[ seen_count++ ], addr, 6 );
  }

  return true;
}

static void print_adv_name( const uint8_t *data, uint8_t len )
{
  uint8_t i = 0;

  while ( i + 1 < len ) {
    uint8_t flen = data[ i ];
    uint8_t type;

    if ( flen == 0 || i + flen >= len + 1u ) {
      return;
    }

    type = data[ i + 1 ];

    /* 0x08 shortened local name, 0x09 complete local name */
    if ( ( type == 0x08 || type == 0x09 ) && flen > 1 ) {
      printk( "  \"" );
      for ( uint8_t k = 0; k < flen - 1; ++k ) {
        uint8_t c = data[ i + 2 + k ];
        printk( "%c", ( c >= 0x20 && c < 0x7f ) ? c : '.' );
      }
      printk( "\"" );
      return;
    }

    i += flen + 1;
  }
}

static void handle_adv_report( const uint8_t *p, uint8_t len )
{
  uint8_t num;

  if ( len < 2 ) {
    return;
  }

  num = p[ 0 ];
  p += 1;
  len -= 1;

  for ( uint8_t n = 0; n < num && len >= 9; ++n ) {
    uint8_t        evt_type = p[ 0 ];
    uint8_t        addr_type = p[ 1 ];
    const uint8_t *addr = &p[ 2 ];
    uint8_t        data_len = p[ 8 ];
    const uint8_t *data = &p[ 9 ];
    int8_t         rssi;

    if ( len < 9u + data_len + 1u ) {
      return;
    }

    rssi = (int8_t) data[ data_len ];

    ++reports;

    if ( remember( addr ) ) {
      /* HCI gives the address least significant byte first. */
      printk(
        "  %02x:%02x:%02x:%02x:%02x:%02x  %s  rssi %4d  %2u bytes",
        addr[ 5 ], addr[ 4 ], addr[ 3 ], addr[ 2 ], addr[ 1 ], addr[ 0 ],
        addr_type == 0 ? "public" : "random",
        rssi, data_len
      );
      print_adv_name( data, data_len );
      printk( "  [evt %u]\n", evt_type );
    }

    p += 9 + data_len + 1;
    len -= 9 + data_len + 1;
  }
}

static void vhci_send_available( void )
{
}

static int vhci_recv( uint8_t *data, uint16_t len )
{
  if ( len < 3 || data[ 0 ] != H4_EVENT ) {
    return 0;
  }

  ++events;

  if ( data[ 1 ] == HCI_EV_CMD_COMPLETE && len >= 7 ) {
    cmd_done_opcode = (uint16_t) ( data[ 4 ] | ( data[ 5 ] << 8 ) );
    cmd_done_status = data[ 6 ];
    cmd_done = true;
  } else if ( data[ 1 ] == HCI_EV_LE_META && len >= 4 &&
              data[ 3 ] == HCI_SUBEV_ADV_REPORT ) {
    handle_adv_report( &data[ 4 ], (uint8_t) ( data[ 2 ] - 1 ) );
  }

  return 0;
}

static const struct {
  void ( *notify_host_send_available )( void );
  int ( *notify_host_recv )( uint8_t *data, uint16_t len );
} vhci_callbacks = { vhci_send_available, vhci_recv };

/* -------------------------------------------------------------------- */

static bool send_hci( const char *what, uint8_t *pkt, uint16_t len,
                      uint16_t opcode )
{
  int waited = 0;

  cmd_done = false;

  while ( !API_vhci_host_check_send_available() ) {
    rtems_task_wake_after( 1 );
    if ( ++waited > 200 ) {
      printk( "%-28s TIMEOUT waiting for VHCI\n", what );
      return false;
    }
  }

  API_vhci_host_send_packet( pkt, len );

  waited = 0;
  while ( !cmd_done ) {
    rtems_task_wake_after( 1 );
    if ( ++waited > 300 ) {
      printk( "%-28s NO COMMAND COMPLETE\n", what );
      return false;
    }
  }

  if ( cmd_done_opcode != opcode ) {
    printk( "%-28s completed opcode 0x%04x, wanted 0x%04x\n",
            what, cmd_done_opcode, opcode );
    return false;
  }

  if ( cmd_done_status != 0 ) {
    printk( "%-28s status 0x%02x\n", what, cmd_done_status );
    return false;
  }

  printk( "%-28s ok\n", what );

  return true;
}

static int failures;

static void expect( const char *what, bool ok )
{
  printk( "%-28s %s\n", what, ok ? "ok" : "FAIL" );
  if ( !ok ) {
    ++failures;
  }
}

extern volatile uint32_t rtems_esp_bt_calls;

#define INT_MATRIX_MAP( src ) \
  ( *( (volatile uint32_t *) (uintptr_t) ( 0x600c2000u + (src) * 4u ) ) )

/*
 * A watchdog on the bring-up.  btdm_controller_enable() either returns or
 * hangs, and when it hangs there is nothing on the console to say why.  This
 * runs below it and reports which adapter entries the controller reached and
 * whether anything routed a Bluetooth interrupt.
 */
static rtems_task Monitor( rtems_task_argument arg )
{
  (void) arg;

  for ( int s = 1; ; ++s ) {
    rtems_task_wake_after( rtems_clock_get_ticks_per_second() * 3 );

    printk( "\n[monitor %ds] adapter calls 0x%03x  matrix 4..10:",
            s * 3, (unsigned) rtems_esp_bt_calls );
    for ( int v = 4; v <= 10; ++v ) {
      printk( " %u", (unsigned) INT_MATRIX_MAP( v ) );
    }
    printk( "\n" );

    if ( s >= 4 ) {
      printk( "[monitor] bring-up did not finish\n" );
      printk( "*** END OF ESP32-C3 BLE SCAN ***\n" );
      exit( 0 );
    }
  }
}

static rtems_task Init( rtems_task_argument arg )
{
  esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

  (void) arg;

  {
    rtems_id mon;

    if ( rtems_task_create( rtems_build_name( 'M', 'O', 'N', ' ' ), 120,
                            8192, RTEMS_DEFAULT_MODES, RTEMS_DEFAULT_ATTRIBUTES,
                            &mon ) == RTEMS_SUCCESSFUL ) {
      rtems_task_start( mon, Monitor, 0 );
    }
  }

  printk( "\n*** ESP32-C3 BLE SCAN ***\n" );
  printk( "config magic 0x%08x version 0x%08x\n",
          (unsigned) cfg.magic, (unsigned) cfg.version );

  if ( rtems_esp_bt_controller_init( &cfg ) != 0 ) {
    printk( "\ncontroller init failed\n" );
    printk( "*** END OF ESP32-C3 BLE SCAN ***\n" );
    exit( 0 );
  }

  if ( rtems_esp_bt_controller_enable() != 0 ) {
    printk( "\ncontroller enable failed\n" );
    printk( "*** END OF ESP32-C3 BLE SCAN ***\n" );
    exit( 0 );
  }

  expect( "register VHCI callbacks",
          API_vhci_host_register_callback( &vhci_callbacks ) == 0 );

  {
    /* HCI Reset, opcode 0x0c03 */
    uint8_t reset[] = { H4_COMMAND, 0x03, 0x0c, 0x00 };

    if ( !send_hci( "HCI Reset", reset, sizeof( reset ), 0x0c03 ) ) {
      ++failures;
    }
  }

  {
    /*
     * LE Set Scan Parameters, opcode 0x200b.
     * Passive scan, interval 0x0060 and window 0x0030 in 0.625 ms units --
     * 60 ms every 96 ms, a 50 percent duty cycle, which is what finds things
     * quickly without a connection to starve.
     */
    uint8_t params[] = {
      H4_COMMAND, 0x0b, 0x20, 0x07,
      0x00,             /* passive */
      0x60, 0x00,       /* interval */
      0x30, 0x00,       /* window */
      0x00,             /* own address type: public */
      0x00              /* filter policy: accept all */
    };

    if ( !send_hci( "LE Set Scan Parameters", params, sizeof( params ),
                    0x200b ) ) {
      ++failures;
    }
  }

  {
    /* LE Set Scan Enable, opcode 0x200c: enable, filter duplicates */
    uint8_t enable[] = { H4_COMMAND, 0x0c, 0x20, 0x02, 0x01, 0x01 };

    if ( !send_hci( "LE Set Scan Enable", enable, sizeof( enable ),
                    0x200c ) ) {
      ++failures;
    }
  }

  printk( "\nscanning for 15 seconds...\n\n" );

  for ( int s = 0; s < 15; ++s ) {
    rtems_task_wake_after( rtems_clock_get_ticks_per_second() );
  }

  {
    /* LE Set Scan Enable: disable */
    uint8_t disable[] = { H4_COMMAND, 0x0c, 0x20, 0x02, 0x00, 0x00 };

    (void) send_hci( "LE Set Scan Disable", disable, sizeof( disable ),
                     0x200c );
  }

  printk( "\n%u distinct advertisers, %u reports, %u HCI events\n",
          (unsigned) seen_count, reports, events );

  expect( "at least one advertiser was heard", seen_count > 0 );

  printk( "\n%d failure(s)\n", failures );
  if ( failures == 0 ) {
    printk( "CI-MARKER ble scan ok\n" );
  }
  printk( "*** END OF ESP32-C3 BLE SCAN ***\n" );

  exit( 0 );
}

#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_MAXIMUM_TASKS 12
#define CONFIGURE_MAXIMUM_SEMAPHORES 32
#define CONFIGURE_MAXIMUM_MESSAGE_QUEUES 16
#define CONFIGURE_MAXIMUM_TIMERS 8
#define CONFIGURE_MESSAGE_BUFFER_MEMORY ( 16 * 1024 )
#define CONFIGURE_UNIFIED_WORK_AREAS
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_STACK_SIZE ( 16 * 1024 )
/*
 * Low priority on purpose.  On FreeRTOS the controller task runs at
 * configMAX_PRIORITIES - 2, far above the application; RTEMS counts the other
 * way and the init task defaults to 1, the highest there is.  Leaving it there
 * puts the controller below the task that is waiting on it, and
 * btdm_controller_enable() never returns.
 */
#define CONFIGURE_INIT_TASK_PRIORITY 100
#define CONFIGURE_INIT_TASK_ATTRIBUTES RTEMS_FLOATING_POINT
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
