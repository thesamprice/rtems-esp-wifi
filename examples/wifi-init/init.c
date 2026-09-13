/*
 * Bring the Espressif WiFi libraries up on RTEMS as far as they will go.
 *
 * What this proves, and what it does not: it links the blobs into a real RTEMS
 * image for the esp32c3db BSP, copies the IRAM sections into the instruction
 * window, installs the OS adapter table and calls into the libraries.  Each
 * step prints before and after, so a fault says which call reached the blobs
 * and which did not.
 *
 * It is not a test of associating with an access point.  QEMU's esp32c3 model
 * has no WiFi MAC, so the PHY and MAC registers the libraries write are not
 * backed by anything; how far this gets under emulation is a statement about
 * the port's software surface, not about the radio.  Reaching
 * esp_wifi_init_internal() at all means the adapter table, the ROM symbols,
 * the section placement and the calibration data are in place, which is the
 * part that can be checked without hardware.
 */

#include <rtems-esp/newlib-compat.h>

#include <esp_wifi.h>
/* esp_wifi.h only forward-declares wifi_osi_funcs_t; the version and magic
 * fields are in the private header. */
#include <esp_private/wifi_os_adapter.h>

#include <rtems.h>
#include <rtems/bspIo.h>

#include <stdio.h>
#include <stdlib.h>

static int failures;

static void check( const char *what, bool ok )
{
  printf( "%-52s %s\n", what, ok ? "ok" : "FAIL" );

  if ( !ok ) {
    ++failures;
  }
}

/*
 * Registered before esp_wifi_init(), because a handler that is not there when
 * the first event is posted never hears it.
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

static rtems_task Init( rtems_task_argument arg )
{
  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t          rv;

  (void) arg;

  printf( "\n*** ESP32-C3 WIFI INIT TEST ***\n" );

  /*
   * The table travels in the config.  Checked here rather than only inside
   * esp_wifi_init() because a null here means the macro and the adapter
   * disagree, which is a build problem and not a runtime one.
   */
  check( "WIFI_INIT_CONFIG_DEFAULT carries the adapter table",
         config.osi_funcs != NULL );
  check( "the adapter table reports the version ESP-IDF expects",
         config.osi_funcs->_version == ESP_WIFI_OS_ADAPTER_VERSION );
  check( "and its magic",
         config.osi_funcs->_magic == ESP_WIFI_OS_ADAPTER_MAGIC );

  rv = esp_event_handler_register( WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   on_wifi_event, NULL );
  check( "esp_event_handler_register", rv == ESP_OK );

  printf( "calling esp_wifi_init...\n" );
  rv = esp_wifi_init( &config );
  printf( "esp_wifi_init returned %i\n", (int) rv );
  check( "esp_wifi_init", rv == ESP_OK );

  if ( rv == ESP_OK ) {
    rv = esp_wifi_set_mode( WIFI_MODE_STA );
    check( "esp_wifi_set_mode(STA)", rv == ESP_OK );

    printf( "calling esp_wifi_start...\n" );
    rv = esp_wifi_start();
    printf( "esp_wifi_start returned %i\n", (int) rv );
    check( "esp_wifi_start", rv == ESP_OK );
  }

  printf( "\n%d failure(s)\n", failures );

  if ( failures == 0 ) {
    printf( "CI-MARKER wifi init ok\n" );
  }

  printf( "*** END OF ESP32-C3 WIFI INIT TEST ***\n" );

  exit( 0 );
}

#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_MAXIMUM_TASKS 16
#define CONFIGURE_MAXIMUM_SEMAPHORES 64
#define CONFIGURE_MAXIMUM_MESSAGE_QUEUES 16
#define CONFIGURE_MAXIMUM_POSIX_KEYS 8
/* The WiFi libraries arm several ETS timers during init and start; 8 ran out
 * and the adapter said so.  Not a defect -- an application-configuration
 * number that the libraries' real demand has now measured. */
#define CONFIGURE_MAXIMUM_TIMERS 32
#define CONFIGURE_MAXIMUM_DRIVERS 8
#define CONFIGURE_UNIFIED_WORK_AREAS
#define CONFIGURE_INIT_TASK_STACK_SIZE ( 16 * 1024 )
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
