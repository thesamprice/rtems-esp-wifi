/*
 * Does the ROM write to the top of the SRAM on the paths this port takes?
 *
 * rtems-esphome#120: the esp32c3db BSP's RAM region ran to 0x3fcd0000, the end
 * of the internal SRAM, while ESP-IDF stops at 0x3fcce710 because the ROM
 * keeps its own data from there up.  The BSP change caps the region; this is
 * the measurement that says whether the 6384 bytes in between are genuinely
 * the ROM's or whether the ESP-IDF ceiling is merely conservative.
 *
 * HOW IT IS SAFE TO WRITE THERE AT ALL
 *
 * Only because the BSP change exists.  With ESP32C_DRAM_ROM_RESERVE_SIZE set,
 * the range from esp32c_dram_rom_reserve_begin to 0x3fcd0000 is outside every
 * region the linker knows about: no section is placed in it and the allocator
 * never hands it out.  So nothing of ours can write there, and anything that
 * changes was written by something that is not us.
 *
 * That is why the window is derived from the linker symbol rather than from a
 * constant.  A literal would keep its value after a rebuild against a BSP that
 * reserves less, and the fill would then be quietly scribbling on the heap --
 * a test that destroys what it is measuring and still prints a verdict.  The
 * checks below refuse to fill unless the window is provably above
 * bsp_section_work_end.
 *
 * WHAT A PASS MEANS, AND WHAT IT DOES NOT
 *
 * A pass says: no ROM path taken by THIS run wrote into that range.  It does
 * not say the ROM never does.  Boot is not covered -- the ROM ran before this
 * program existed -- and neither is any path the run did not take.
 *
 * Which is why "the station associated" is a check and not a note.  The paths
 * worth the most here are the PHY calibration and the ROM halves of libpp and
 * libnet80211 ("pp rom version: 9387209", "net80211 rom version: 9387209"),
 * and those run during scan, authentication and the four-way handshake.  A run
 * that never associated exercises a fraction of them, so a clean window from
 * such a run is not evidence and this test declines to call it one.  Under
 * QEMU there is no WiFi MAC and association cannot happen, so this test is a
 * hardware test and says so rather than passing vacuously.
 *
 * SHOWING IT CAN FAIL
 *
 * Two ways, both cheap and both required before believing a clean run:
 *
 *   Build it against a BSP with no ROM reserve.  esp32c_dram_rom_reserve_begin
 *   is referenced weakly, so the link still succeeds and the run reports that
 *   the range is inside the heap and refuses to fill.  That is the unpatched
 *   BSP, and it is how the first check was shown to have teeth.
 *
 *   Build it with -DROM_OVERLAP_SELF_TEST.  One word in the window is
 *   corrupted after the fill, by a store indistinguishable from the ones being
 *   looked for, and the run must report exactly that word at exactly that
 *   address.  That is how the read-back half was shown to have teeth.
 */

#include <rtems-esp/newlib-compat.h>

#include <esp_wifi.h>
/* esp_wifi.h only forward-declares wifi_osi_funcs_t; the version and magic
 * fields are in the private header. */
#include <esp_private/wifi_os_adapter.h>

#include <rtems.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The network to associate with.
 *
 * Placeholders, and they have to be overridden at build time -- credentials
 * never reach a commit.  Unlike examples/wifi-net there is no useful empty
 * password default here: an open access point does no four-way handshake, and
 * the handshake is one of the ROM paths this exists to exercise.
 */
#ifndef ROM_OVERLAP_SSID
#define ROM_OVERLAP_SSID "rtems-test-network"
#endif
#ifndef ROM_OVERLAP_PASSWORD
#define ROM_OVERLAP_PASSWORD ""
#endif

/*
 * The end of the internal SRAM, which is also the end of the window.  This one
 * is a literal because it is the edge of the silicon: there is no region above
 * it for a symbol to come from, and it does not move.
 */
#define SRAM_END 0x3fcd0000u

/*
 * Where the ROM's data begins according to ESP-IDF, whose esp_system/ld/
 * memory.ld derives dram0_0_seg's length from 0x403ce710 -- the same address
 * seen through the instruction window, 0x700000 higher.
 *
 * A literal on purpose, and the only one that is a specification rather than
 * an observation.  The BSP's reserve is checked AGAINST it below; if the two
 * were the same symbol that check could not exist.
 */
#define IDF_ROM_DATA_BEGIN 0x3fcce710u

/*
 * The first address the BSP gave back to the ROM.
 *
 * Weak, so that this program links against a BSP that has no reserve at all
 * and can then say so.  A strong reference would turn "the BSP predates the
 * fix" into a link error -- which is a legitimate way to build a test, but not
 * this one: the unpatched BSP is the negative control and it has to be
 * runnable.
 */
extern char esp32c_dram_rom_reserve_begin[] __attribute__( ( weak ) );

/* Where the work area stops, which is the top of everything the allocator may
 * hand out.  Always present; from the BSP's linker script. */
extern char bsp_section_work_end[];

/* How long to give the association and the traffic after it, in seconds. */
#define ASSOCIATE_WAIT_SECONDS 25

static int failures;

static void check( const char *what, bool ok )
{
  printf( "%-56s %s\n", what, ok ? "ok" : "FAIL" );

  if ( !ok ) {
    ++failures;
  }
}

/*
 * The pattern is derived from the address it is stored at, not constant.
 *
 * A constant fill answers "was this overwritten"; an address-derived one also
 * answers "was this moved".  A ROM routine that memcpy's a block around inside
 * the window would leave a constant fill looking untouched, and that is
 * precisely one of the things worth catching.
 */
static uint32_t pattern_for( uintptr_t addr )
{
  return (uint32_t) addr ^ 0x5a5aa5a5u;
}

static volatile uint32_t *window_begin;
static volatile uint32_t *window_end;

static void fill_window( void )
{
  volatile uint32_t *p;

  for ( p = window_begin; p < window_end; ++p ) {
    *p = pattern_for( (uintptr_t) p );
  }
}

/*
 * Walk the window and report what changed.
 *
 * The count alone would not answer the question the issue asks -- "the
 * survivors tell us how far down it reaches" -- so the lowest and highest
 * damaged addresses are what this is really for, and the first few words are
 * printed because a plausible value (a pointer, ASCII, a small integer) says
 * more about who wrote it than the address does.
 */
static int verify_window( uintptr_t *lowest, uintptr_t *highest )
{
  volatile uint32_t *p;
  int                damaged = 0;
  int                shown = 0;

  *lowest = 0;
  *highest = 0;

  for ( p = window_begin; p < window_end; ++p ) {
    uint32_t want = pattern_for( (uintptr_t) p );
    uint32_t got = *p;

    if ( got == want ) {
      continue;
    }

    ++damaged;

    if ( *lowest == 0 ) {
      *lowest = (uintptr_t) p;
    }

    *highest = (uintptr_t) p;

    if ( shown < 16 ) {
      printf( "       changed at 0x%08" PRIxPTR ": wrote %08x, read %08x\n",
              (uintptr_t) p, (unsigned) want, (unsigned) got );
      ++shown;
    }
  }

  if ( damaged > shown ) {
    printf( "       ... and %d more\n", damaged - shown );
  }

  return damaged;
}

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

  switch ( id ) {
    case WIFI_EVENT_STA_START:     ++sta_start_seen;     break;
    case WIFI_EVENT_STA_CONNECTED: ++sta_connected_seen; break;
    case WIFI_EVENT_STA_DISCONNECTED:
      ++sta_disconnected_seen;

      /*
       * The reason, because a bare disconnect does not distinguish "the
       * network was never heard" from "the passphrase was wrong", and those
       * mean two different things about how much of the ROM ran.  201 is
       * WIFI_REASON_NO_AP_FOUND, which on this chip usually means a 5GHz
       * network; 15 is WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT.
       */
      if ( data != NULL ) {
        const wifi_event_sta_disconnected_t *d = data;

        printf( "       disconnected: reason %i, rssi %i\n",
                (int) d->reason, (int) d->rssi );
      }

      break;
    default: break;
  }
}

static rtems_task Init( rtems_task_argument arg )
{
  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t          rv;
  uintptr_t          reserve;
  uintptr_t          work_end = (uintptr_t) bsp_section_work_end;
  size_t             bytes;
  int                damaged;
  uintptr_t          lowest;
  uintptr_t          highest;
  bool               fillable;

  (void) arg;

  printf( "\n*** ESP32-C3 ROM OVERLAP TEST ***\n" );

  reserve = (uintptr_t) esp32c_dram_rom_reserve_begin;

  printf( "work area ends at      0x%08" PRIxPTR "\n", work_end );
  printf( "BSP reserve begins at  0x%08" PRIxPTR "%s\n", reserve,
          esp32c_dram_rom_reserve_begin == NULL ? "  (no such symbol)" : "" );
  printf( "ESP-IDF says ROM data  0x%08x\n", IDF_ROM_DATA_BEGIN );
  printf( "SRAM ends at           0x%08x\n", SRAM_END );

  /*
   * The check the BSP change is about, stated as an address rather than as an
   * outcome (CONTRIBUTING.md, "assert addresses and identities").  Against an
   * unpatched BSP work_end is 0x3fcd0000 and this fails, which is the whole of
   * the negative control for the linker half.
   */
  check( "the work area stops at or below the ROM's data",
         work_end <= IDF_ROM_DATA_BEGIN );

  /*
   * "Reserves nothing" and "reserves a different amount" are different
   * answers and a literal could give neither.  A BSP with no symbol at all is
   * one that predates the change.
   */
  check( "the BSP exports a ROM reserve boundary",
         esp32c_dram_rom_reserve_begin != NULL );

  if ( esp32c_dram_rom_reserve_begin != NULL ) {
    check( "and it is at or below ESP-IDF's boundary",
           reserve <= IDF_ROM_DATA_BEGIN );
  }

  /*
   * Refuse to fill unless the window is provably nobody's.  Getting this wrong
   * does not produce a wrong answer, it produces a corrupted heap and then a
   * wrong answer, and the second would be blamed on the ROM.
   */
  fillable = esp32c_dram_rom_reserve_begin != NULL && reserve >= work_end &&
             reserve < SRAM_END;

  check( "the window is above the work area, so filling it is safe",
         fillable );

  if ( !fillable ) {
    printf(
      "\nNot filling anything.  This range is inside the heap in this image,\n"
      "so writing the pattern would corrupt whatever is allocated there and\n"
      "the read-back would be measuring this test rather than the ROM.\n"
      "Rebuild against a BSP with ESP32C_DRAM_ROM_RESERVE_SIZE set.\n" );
    goto done;
  }

  window_begin = (volatile uint32_t *) reserve;
  window_end = (volatile uint32_t *) (uintptr_t) SRAM_END;
  bytes = (size_t) ( SRAM_END - reserve );

  printf( "\nwindow 0x%08" PRIxPTR " .. 0x%08x, %zu bytes\n",
          reserve, SRAM_END, bytes );

  /*
   * How much of the window is below ESP-IDF's boundary matters when reading
   * the result.  Those bytes are ours by ESP-IDF's own arithmetic, so damage
   * there is a stronger claim than damage above it: it would mean the ROM
   * reaches further down than ESP-IDF believes, and it is the number the issue
   * asks for.  With the default reserve there are none of them and the window
   * is exactly the disputed range.
   */
  if ( reserve < IDF_ROM_DATA_BEGIN ) {
    printf( "  of which %u bytes are BELOW ESP-IDF's boundary and are\n"
            "  expected to survive; damage there is the interesting result\n",
            (unsigned) ( IDF_ROM_DATA_BEGIN - reserve ) );
  } else {
    printf( "  which is exactly the disputed range; nothing below the\n"
            "  ESP-IDF boundary is being watched in this build\n" );
  }

  fill_window();

  /* The fill is only worth anything if it is really there.  A read-back of one
   * word costs nothing and separates "survived" from "never written". */
  check( "the pattern reads back before the radio is touched",
         *window_begin == pattern_for( (uintptr_t) window_begin ) &&
           *( window_end - 1 ) ==
             pattern_for( (uintptr_t) ( window_end - 1 ) ) );

#ifdef ROM_OVERLAP_SELF_TEST
  /*
   * The negative control for the read-back half.  One word, written the same
   * way a ROM store would write it, at an address the report below must name.
   */
  printf( "\nSELF TEST: corrupting one word at 0x%08" PRIxPTR "\n",
          (uintptr_t) ( window_begin + 64 ) );
  window_begin[ 64 ] = 0xdeadbeefu;
#endif

  /* ---- the radio, which is what runs the ROM ---- */

  check( "WIFI_INIT_CONFIG_DEFAULT carries the adapter table",
         config.osi_funcs != NULL );
  check( "the adapter table reports the version ESP-IDF expects",
         config.osi_funcs->_version == ESP_WIFI_OS_ADAPTER_VERSION );

  rv = esp_event_handler_register( WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   on_wifi_event, NULL );
  check( "esp_event_handler_register", rv == ESP_OK );

  printf( "\ncalling esp_wifi_init...\n" );
  rv = esp_wifi_init( &config );
  check( "esp_wifi_init", rv == ESP_OK );

  if ( rv != ESP_OK ) {
    goto measure;
  }

  rv = esp_wifi_set_mode( WIFI_MODE_STA );
  check( "esp_wifi_set_mode(STA)", rv == ESP_OK );

  /*
   * Before esp_wifi_start(), not after.  esp_wifi_connect() reads the
   * configuration the station was started with; setting it later is accepted
   * and then ignored, which presents as a connect that attempts nothing.
   */
  {
    wifi_config_t sta = { 0 };

    strncpy( (char *) sta.sta.ssid, ROM_OVERLAP_SSID,
             sizeof( sta.sta.ssid ) - 1 );
    strncpy( (char *) sta.sta.password, ROM_OVERLAP_PASSWORD,
             sizeof( sta.sta.password ) - 1 );
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    rv = esp_wifi_set_config( WIFI_IF_STA, &sta );
    check( "esp_wifi_set_config(STA)", rv == ESP_OK );
  }

  printf( "calling esp_wifi_start...\n" );
  rv = esp_wifi_start();
  check( "esp_wifi_start", rv == ESP_OK );

  /* The event task runs at priority 100 and Init at 1, so nothing dispatched
   * from it can have run yet.  Blocking here is what lets it. */
  rtems_task_wake_after( 2 * rtems_clock_get_ticks_per_second() );
  check( "STA_START was delivered to the handler", sta_start_seen > 0 );

  printf( "calling esp_wifi_connect to %s...\n", ROM_OVERLAP_SSID );
  rv = esp_wifi_connect();
  check( "esp_wifi_connect was accepted", rv == ESP_OK );

  /*
   * Stay associated for a while rather than reading back the moment the
   * CONNECTED event arrives.  Beacons, the rate control and the ROM half of
   * libpp keep running while the link is up, and a window read back one
   * millisecond after association has not seen any of that.
   */
  {
    int i;

    for ( i = 0; i < ASSOCIATE_WAIT_SECONDS; ++i ) {
      rtems_task_wake_after( rtems_clock_get_ticks_per_second() );
    }
  }

measure:
  printf( "\nassociation: %d connected, %d disconnected\n",
          sta_connected_seen, sta_disconnected_seen );

  /*
   * The guard against a measurement that did not run.  An untouched window
   * from a station that never associated is not evidence about the ROM; it is
   * evidence that the scan ran and little else.  Under QEMU this is the check
   * that fails, correctly -- there is no WiFi MAC to associate with.
   */
  check( "the station associated, so the ROM paths actually ran",
         sta_connected_seen > 0 );

  printf( "\nreading the window back...\n" );
  damaged = verify_window( &lowest, &highest );

  printf( "%d word(s) changed out of %u\n", damaged,
          (unsigned) ( bytes / sizeof( uint32_t ) ) );

  if ( damaged > 0 ) {
    printf( "lowest  changed 0x%08" PRIxPTR "\n", lowest );
    printf( "highest changed 0x%08" PRIxPTR "\n", highest );
    printf( "so something reaches down to 0x%08" PRIxPTR
            ", %u bytes below the top\n",
            lowest, (unsigned) ( SRAM_END - lowest ) );
  }

#ifdef ROM_OVERLAP_SELF_TEST
  /*
   * In self-test mode the verdict is inverted: the detector has to find the
   * planted word, at its address.  If it does not, the read-back cannot be
   * trusted to report a real write either, and a clean run from the same
   * binary would mean nothing.
   */
  check( "the self test's corruption was detected",
         damaged >= 1 && lowest == (uintptr_t) ( window_begin + 64 ) );
#else
  check( "the window survived every ROM path this run took",
         damaged == 0 );
#endif

done:
  printf( "\n%d failure(s)\n", failures );

  if ( failures == 0 ) {
#ifdef ROM_OVERLAP_SELF_TEST
    printf( "CI-MARKER rom-overlap self-test detected\n" );
#else
    /*
     * Deliberately not "the ROM does not use this memory".  It says what was
     * measured: one run, the paths that run took, nothing written.
     */
    printf( "CI-MARKER rom-overlap clean on the paths this run took\n" );
#endif
  }

  printf( "*** END OF ESP32-C3 ROM OVERLAP TEST ***\n" );

  exit( 0 );
}

#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_MAXIMUM_TASKS 16
#define CONFIGURE_MAXIMUM_SEMAPHORES 64
#define CONFIGURE_MAXIMUM_MESSAGE_QUEUES 16
#define CONFIGURE_MAXIMUM_POSIX_KEYS 8
/* The WiFi libraries arm several ETS timers during init and start; 8 ran out
 * and the adapter said so.  See examples/wifi-init. */
#define CONFIGURE_MAXIMUM_TIMERS 32
#define CONFIGURE_MAXIMUM_DRIVERS 8
#define CONFIGURE_UNIFIED_WORK_AREAS
#define CONFIGURE_INIT_TASK_STACK_SIZE ( 16 * 1024 )
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
