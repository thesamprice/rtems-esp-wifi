/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * The ESP32-C3 BLE controller on RTEMS.
 *
 * libbtdm_app.a is thinner than it looks.  Of its 1369 undefined symbols,
 * 1368 are satisfied by the part's own ROM -- esp32c3.rom.bt_funcs.ld alone
 * names most of the Riviera Waves link layer -- by libbtbb.a and by libphy.a.
 * The single remaining one is ets_delay_us.  So almost nothing has to be
 * reimplemented; what has to be supplied is a runtime contract rather than
 * code the linker is missing.
 *
 * That contract is osi_funcs_t: a table of about sixty function pointers the
 * controller calls for tasks, queues, semaphores, mutexes, interrupts, time
 * and memory.  ESP-IDF fills it in components/bt/controller/esp32c3/bt.c
 * against FreeRTOS; this fills the same table against RTEMS, and the
 * primitives are the ones rtems_wifi_os_adapter.c already worked out for the
 * WiFi blobs on this chip.
 *
 * Sleep and coexistence are deliberately absent.  Sleep is off in the
 * configuration, so the six _btdm_sleep_* entries are never called; coex
 * matters only with WiFi running, which this does not do.  Both are stubbed
 * and the stubs report if they are ever reached, because a silent stub in a
 * table like this is how you spend a day on a hang.
 */

#include <bsp.h>
#include <bsp/bt.h>
#include <bsp/irq.h>

#include <rtems.h>
#include <rtems/bspIo.h>
#include <rtems/counter.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rtems-esp/newlib-compat.h"

/* -------------------------------------------------------------------- */
/* What the controller exports                                          */
/* -------------------------------------------------------------------- */

#define OSI_VERSION     0x0001000B
#define OSI_MAGIC_VALUE 0xFADEBEAD

extern int  btdm_osi_funcs_register( void *osi_funcs );
extern int  btdm_controller_init( void *config_opts );
extern void btdm_controller_deinit( void );
extern int  btdm_controller_enable( int mode );
extern void btdm_controller_disable( void );
/*
 * btdm_rf_bb_init_phase2() is declared in ESP-IDF's esp32c3/bt.c and never
 * called there -- it belongs to the esp32/esp32s3 controller and does not
 * exist in this blob or the ROM.  What the C3 needs after the PHY is
 * bt_bb_v2_init_cmplx(), which is what esp_phy_enable( PHY_MODEM_BT ) reaches
 * through components/esp_phy/src/btbb_init.c.
 */
extern void bt_bb_v2_init_cmplx( int print_version );
extern void sdk_config_extend_set_pll_track( bool enable );
extern void btdm_controller_enable_sleep( bool enable );
extern bool btdm_lpclk_select_src( uint32_t sel );
extern bool btdm_lpclk_set_div( uint32_t div );
extern void btdm_controller_rom_data_init( void );
extern const char *btdm_controller_get_compile_version( void );

extern bool API_vhci_host_check_send_available( void );
extern void API_vhci_host_send_packet( uint8_t *data, uint16_t len );
extern int  API_vhci_host_register_callback( const void *callback );

/* libphy.a */
extern int register_chipv7_phy( const void *init_data, void *cal_data, int mode );
extern void phy_bbpll_en_usb( bool en );

/* ROM */
extern void ets_update_cpu_frequency( uint32_t mhz );

/*
 * The link layer's own interrupt handlers, which live in ROM.
 *
 * ESP-IDF reaches these through the controller calling _interrupt_alloc in
 * the OS adapter.  This blob never makes that call -- instrumenting all 61
 * entries showed only _semphr_take and _queue_send used during init, and the
 * interrupt matrix entries for sources 4 to 10 stayed clear -- so the handlers
 * are installed here instead.
 *
 * Without them the bring-up deadlocks in a circle: the controller task blocks
 * taking a semaphore, the only thing that gives it is the link layer ISR, the
 * ISR is never routed, and btdm_controller_enable() waits for the controller
 * task.
 */
extern void r_rwbtdm_isr_wrapper( void );
extern void r_bt_bb_isr( void );

/* components/esp_phy/esp32c3/phy_init_data.c, built into phy_init_data.o */
extern const uint8_t phy_init_data[];

#define ESP_CAL_DATA_CHECK_FAIL 1
#define PHY_RF_CAL_FULL         2

/* esp_phy_calibration_data_t: 1904 bytes, mac at offset 0 is not true -- see
 * the struct in esp_phy_init.h.  Only the size and the mac field matter here
 * and both come from that header. */
typedef struct {
  uint8_t version[ 4 ];
  uint8_t mac[ 6 ];
  uint8_t opaque[ 1894 ];
} bt_phy_cal_data_t;

static bt_phy_cal_data_t bt_cal_data;

/*
 * The controller's .iram1 and .dram1 sections, placed by
 * ld/esp32c3-wifi-sections.ld -- the same fragment the WiFi blobs use, which
 * already claims those two names.
 *
 * They are loaded from flash and have to be copied at startup.
 * bsp_start_copy_sections() only knows the BSP's own .fast_text, so nothing
 * moves these unless this does.  Skipping it is not a quiet failure and not a
 * loud one either: the controller runs until the first call into an IRAM
 * function and then takes an illegal instruction from uninitialised SRAM,
 * which is what this port did before this existed.
 */
extern char rtems_esp_wifi_iram_begin[];
extern char rtems_esp_wifi_iram_end[];
extern char rtems_esp_wifi_iram_load_begin[];
extern char rtems_esp_wifi_dram_begin[];
extern char rtems_esp_wifi_dram_end[];
extern char rtems_esp_wifi_dram_load_begin[];
extern char esp32c_iram_to_dram_delta[];

#define FLASH_MAPPED_BASE 0x3c000000

static void bt_copy_sections( void )
{
  size_t    iram_size = (size_t) ( rtems_esp_wifi_iram_end
                                     - rtems_esp_wifi_iram_begin );
  size_t    dram_size = (size_t) ( rtems_esp_wifi_dram_end
                                     - rtems_esp_wifi_dram_begin );
  uintptr_t delta     = (uintptr_t) esp32c_iram_to_dram_delta;

  if ( iram_size != 0 ) {
    /* Through the data window: the two windows address the same SRAM and
     * stores are only guaranteed through the data bus. */
    memcpy(
      (void *) ( (uintptr_t) rtems_esp_wifi_iram_begin + delta ),
      (const void *) ( (uintptr_t) rtems_esp_wifi_iram_load_begin
                         + FLASH_MAPPED_BASE ),
      iram_size
    );
  }

  if ( dram_size != 0 ) {
    memcpy(
      rtems_esp_wifi_dram_begin,
      (const void *) ( (uintptr_t) rtems_esp_wifi_dram_load_begin
                         + FLASH_MAPPED_BASE ),
      dram_size
    );
  }

  printk( "rtems-esp-bt: copied %u bytes of IRAM and %u of DRAM\n",
          (unsigned) iram_size, (unsigned) dram_size );
}

/* -------------------------------------------------------------------- */
/* Registers                                                            */
/* -------------------------------------------------------------------- */

#define REG( a ) ( *(volatile uint32_t *) (uintptr_t) ( a ) )

#define EFUSE_MAC0_REG 0x60008844u
#define EFUSE_MAC1_REG 0x60008848u

static void bt_delay_us_early( uint32_t us )
{
  rtems_counter_ticks start = rtems_counter_read();
  rtems_counter_ticks want = rtems_counter_nanoseconds_to_ticks( us * 1000u );

  while ( rtems_counter_read() - start < want ) {
    /* wait */
  }
}

#define SYSCON_BASE_ADDR   0x60026000u
#define CLK_EN_REG         ( SYSCON_BASE_ADDR + 0x014u )
#define RST_EN_REG         ( SYSCON_BASE_ADDR + 0x018u )
#define RTC_BASE_ADDR      0x60008000u
#define RTC_DIG_PWC_REG    ( RTC_BASE_ADDR + 0x088u )
#define RTC_DIG_ISO_REG    ( RTC_BASE_ADDR + 0x08Cu )

#define WIFI_FORCE_PD      ( 1u << 17 )
#define WIFI_FORCE_PU      ( 1u << 18 )
#define WIFI_PD_EN         ( 1u << 30 )
#define WIFI_FORCE_ISO     ( 1u << 28 )
#define WIFI_FORCE_NOISO   ( 1u << 29 )

#define ANA_CONFIG_REG        0x6000E044u
#define ANA_CONFIG2_REG       0x6000E048u
#define ANA_CONFIG_M          ( 0x3FFu << 8 )
#define ANA_I2C_SAR_FORCE_PD  ( 1u << 18 )
#define ANA_I2C_SAR_FORCE_PU  ( 1u << 16 )

#define CPU_PER_CONF_REG      0x600C0008u
#define SYSCLK_CONF_REG       0x600C0058u

#define CLK_WIFI_BT_COMMON 0x78078Fu
#define CLK_PHY_EN         0x400000u
#define MODEM_RESET_WHEN_PU                                                   \
  ( ( 1u << 0 ) | ( 1u << 1 ) | ( 1u << 2 ) | ( 1u << 3 ) |                    \
    ( 1u << 4 ) | ( 1u << 9 ) | ( 1u << 11 ) | ( 1u << 13 ) )

/*
 * The modem digital block, which is not the Bluetooth block.
 *
 * bsp_esp32_bt_enable() powers the Bluetooth domain.  register_chipv7_phy()
 * needs something else: the front end, the AGC and the baseband, which sit in
 * the domain RTC_CNTL calls WiFi and which both radios share.  ESP-IDF's name
 * for bringing it up, esp_wifi_bt_power_domain_on(), says as much; it is
 * called from the Bluetooth path too.
 *
 * Skipping it does not fail visibly.  register_chipv7_phy() runs into a block
 * that reads back as zero and spins inside it, and the only symptom on the
 * console is libphy printing carriage returns forever.  That is what this
 * port did on its first run.
 *
 * Lifted from rtems_esp_wifi_init.c, whose comments explain why the positive
 * FORCE_PU and FORCE_NOISO bits are set rather than only their opposites
 * cleared: on this boot path they do not hold their reset value, and clearing
 * the negatives alone leaves the domain under automatic control and dark.
 */
static void bt_modem_domain_on( void )
{
  uint32_t reg;

  reg = REG( RTC_DIG_PWC_REG );
  reg &= ~( WIFI_FORCE_PD | WIFI_PD_EN );
  reg |= WIFI_FORCE_PU;
  REG( RTC_DIG_PWC_REG ) = reg;

  bt_delay_us_early( 10u );

  reg = REG( CLK_EN_REG );
  REG( CLK_EN_REG ) = reg | CLK_WIFI_BT_COMMON;

  reg = REG( RST_EN_REG );
  REG( RST_EN_REG ) = reg | MODEM_RESET_WHEN_PU;
  REG( RST_EN_REG ) = reg & ~MODEM_RESET_WHEN_PU;

  reg = REG( RTC_DIG_ISO_REG );
  reg &= ~WIFI_FORCE_ISO;
  reg |= WIFI_FORCE_NOISO;
  REG( RTC_DIG_ISO_REG ) = reg;

  /* The PHY's own clock, which the power-up sequence does not leave on. */
  reg = REG( CLK_EN_REG );
  REG( CLK_EN_REG ) = reg | CLK_WIFI_BT_COMMON | CLK_PHY_EN;

  /*
   * The internal analog I2C master, which is how libphy reaches the analog
   * front end.  ANA_CONFIG bits [17:8] are a disable mask over ten targets,
   * so clearing them makes every target reachable; SAR_FORCE_PU gates the
   * receive path's analog.  Without this the calibration has a digital block
   * to talk to and no analog one behind it.
   */
  reg = REG( ANA_CONFIG_REG );
  reg &= ~ANA_CONFIG_M;
  reg &= ~ANA_I2C_SAR_FORCE_PD;
  REG( ANA_CONFIG_REG ) = reg;

  reg = REG( ANA_CONFIG2_REG );
  REG( ANA_CONFIG2_REG ) = reg | ANA_I2C_SAR_FORCE_PU;

  /*
   * And the CPU onto the BBPLL at 160 MHz.
   *
   * A direct-boot image has no second stage, so nothing has run
   * rtc_clk_init() and the CPU is still on the crystal.  The PHY calibration
   * is written against a 160 MHz core and its timing loops do not converge at
   * 40; this is the difference between a calibration that finishes and one
   * that spins.
   */
  reg = REG( CPU_PER_CONF_REG );
  REG( CPU_PER_CONF_REG ) = ( reg & ~0x3u ) | 1u;

  reg = REG( SYSCLK_CONF_REG );
  reg &= ~0x3FFu;
  reg = ( reg & ~( 0x3u << 10 ) ) | ( 1u << 10 );
  REG( SYSCLK_CONF_REG ) = reg;

  ets_update_cpu_frequency( 160u );

  printk( "rtems-esp-bt: FE[0x174] %08x AGC[0x8c] %08x sysclk %08x\n",
          (unsigned) REG( 0x60006174u ), (unsigned) REG( 0x6001C08Cu ),
          (unsigned) REG( SYSCLK_CONF_REG ) );
}

/* -------------------------------------------------------------------- */
/* Primitives                                                           */
/* -------------------------------------------------------------------- */

/* Diagnostics: which osi_funcs entries the controller has reached. */
extern volatile uint32_t rtems_esp_bt_calls;
#define BT_CALL_SEM_TAKE   ( 1u << 0 )
#define BT_CALL_SEM_GIVE   ( 1u << 1 )
#define BT_CALL_Q_SEND     ( 1u << 2 )
#define BT_CALL_Q_RECV     ( 1u << 3 )
#define BT_CALL_Q_SEND_ISR ( 1u << 4 )
#define BT_CALL_ISR_ALLOC  ( 1u << 5 )
#define BT_CALL_MUTEX      ( 1u << 6 )
#define BT_CALL_IS_IN_ISR  ( 1u << 7 )
#define BT_CALL_SW_INTR    ( 1u << 8 )
#define BT_CALL_RAND       ( 1u << 9 )

#define BT_MAX_OBJECTS 24

typedef struct {
  bool           used;
  rtems_id       id;
  uint32_t       item_size;
} bt_queue_t;

static bt_queue_t bt_queues[ BT_MAX_OBJECTS ];
static rtems_id   bt_sem_ids[ BT_MAX_OBJECTS ];
static bool       bt_sem_used[ BT_MAX_OBJECTS ];

static uint32_t bt_name_counter;

static rtems_name bt_next_name( char a )
{
  uint32_t n = bt_name_counter++;

  return rtems_build_name( a, 'B', (char) ( 'a' + ( n / 26 ) % 26 ),
                           (char) ( 'a' + n % 26 ) );
}

static uint32_t bt_ms_to_ticks( uint32_t ms )
{
  uint32_t per_tick;

  if ( ms == 0xffffffffu ) {
    return RTEMS_NO_TIMEOUT;
  }

  per_tick = (uint32_t) rtems_configuration_get_milliseconds_per_tick();

  if ( per_tick == 0 ) {
    per_tick = 1;
  }

  return ( ms + per_tick - 1 ) / per_tick;
}

static void *bt_semphr_create( uint32_t max, uint32_t init )
{
  rtems_status_code sc;
  rtems_id          id;

  for ( int i = 0; i < BT_MAX_OBJECTS; ++i ) {
    if ( bt_sem_used[ i ] ) {
      continue;
    }

    sc = rtems_semaphore_create(
      bt_next_name( 'S' ),
      init,
      RTEMS_COUNTING_SEMAPHORE | RTEMS_FIFO,
      0,
      &id
    );

    if ( sc != RTEMS_SUCCESSFUL ) {
      printk( "rtems-esp-bt: semaphore_create failed (%d)\n", sc );
      return NULL;
    }

    (void) max;
    bt_sem_used[ i ] = true;
    bt_sem_ids[ i ] = id;

    return &bt_sem_ids[ i ];
  }

  printk( "rtems-esp-bt: out of semaphore slots\n" );

  return NULL;
}

static void bt_semphr_delete( void *semphr )
{
  rtems_id *slot = semphr;

  if ( slot == NULL ) {
    return;
  }

  rtems_semaphore_delete( *slot );
  bt_sem_used[ slot - bt_sem_ids ] = false;
}

static int bt_semphr_take( void *semphr, uint32_t block_time_ms )
{
  rtems_esp_bt_calls |= BT_CALL_SEM_TAKE;
  rtems_id *slot = semphr;

  if ( rtems_semaphore_obtain( *slot, RTEMS_WAIT,
                               bt_ms_to_ticks( block_time_ms ) )
       == RTEMS_SUCCESSFUL ) {
    return 1;
  }

  return 0;
}

static int bt_semphr_give( void *semphr )
{
  rtems_esp_bt_calls |= BT_CALL_SEM_GIVE;
  rtems_id *slot = semphr;

  return rtems_semaphore_release( *slot ) == RTEMS_SUCCESSFUL ? 1 : 0;
}

static int bt_semphr_take_from_isr( void *semphr, void *hptw )
{
  *(bool *) hptw = false;

  return rtems_semaphore_obtain( *(rtems_id *) semphr, RTEMS_NO_WAIT, 0 )
         == RTEMS_SUCCESSFUL ? 1 : 0;
}

static int bt_semphr_give_from_isr( void *semphr, void *hptw )
{
  *(bool *) hptw = false;

  return bt_semphr_give( semphr );
}

static void *bt_mutex_create( void )
{
  return bt_semphr_create( 1, 1 );
}

static void bt_mutex_delete( void *mutex )
{
  bt_semphr_delete( mutex );
}

static int bt_mutex_lock( void *mutex )
{
  rtems_esp_bt_calls |= BT_CALL_MUTEX;
  return bt_semphr_take( mutex, 0xffffffffu ) ? 0 : -1;
}

static int bt_mutex_unlock( void *mutex )
{
  return bt_semphr_give( mutex ) ? 0 : -1;
}

static void *bt_queue_create( uint32_t queue_len, uint32_t item_size )
{
  rtems_status_code sc;

  for ( int i = 0; i < BT_MAX_OBJECTS; ++i ) {
    if ( bt_queues[ i ].used ) {
      continue;
    }

    sc = rtems_message_queue_create(
      bt_next_name( 'Q' ),
      queue_len,
      item_size,
      RTEMS_FIFO,
      &bt_queues[ i ].id
    );

    if ( sc != RTEMS_SUCCESSFUL ) {
      printk( "rtems-esp-bt: message_queue_create(%u,%u) failed (%d)\n",
              (unsigned) queue_len, (unsigned) item_size, sc );
      return NULL;
    }

    bt_queues[ i ].used = true;
    bt_queues[ i ].item_size = item_size;

    printk( "rtems-esp-bt: queue %d: %u x %u bytes\n", i,
            (unsigned) queue_len, (unsigned) item_size );

    return &bt_queues[ i ];
  }

  printk( "rtems-esp-bt: out of queue slots\n" );

  return NULL;
}

static void bt_queue_delete( void *queue )
{
  bt_queue_t *q = queue;

  if ( q == NULL ) {
    return;
  }

  rtems_message_queue_delete( q->id );
  q->used = false;
}

static int bt_queue_send( void *queue, void *item, uint32_t block_time_ms )
{
  rtems_esp_bt_calls |= BT_CALL_Q_SEND;
  bt_queue_t *q = queue;

  (void) block_time_ms;

  return rtems_message_queue_send( q->id, item, q->item_size )
         == RTEMS_SUCCESSFUL ? 1 : 0;
}

static int bt_queue_send_from_isr( void *queue, void *item, void *hptw )
{
  rtems_esp_bt_calls |= BT_CALL_Q_SEND_ISR;
  *(bool *) hptw = false;

  return bt_queue_send( queue, item, 0 );
}

static int bt_queue_recv( void *queue, void *item, uint32_t block_time_ms )
{
  rtems_esp_bt_calls |= BT_CALL_Q_RECV;
  bt_queue_t *q = queue;
  size_t      size;

  return rtems_message_queue_receive( q->id, item, &size, RTEMS_WAIT,
                                      bt_ms_to_ticks( block_time_ms ) )
         == RTEMS_SUCCESSFUL ? 1 : 0;
}

static int bt_queue_recv_from_isr( void *queue, void *item, void *hptw )
{
  bt_queue_t *q = queue;
  size_t      size;

  *(bool *) hptw = false;

  return rtems_message_queue_receive( q->id, item, &size, RTEMS_NO_WAIT, 0 )
         == RTEMS_SUCCESSFUL ? 1 : 0;
}

typedef void ( *bt_task_entry )( void *arg );

typedef struct {
  bt_task_entry entry;
  void         *arg;
} bt_task_start_t;

static bt_task_start_t bt_task_starts[ 4 ];
static int             bt_task_start_count;

static rtems_task bt_task_trampoline( rtems_task_argument arg )
{
  bt_task_start_t *s = &bt_task_starts[ arg ];

  printk( "rtems-esp-bt: controller task running\n" );
  ( *s->entry )( s->arg );
  printk( "rtems-esp-bt: controller task returned\n" );

  rtems_task_exit();
}

static int bt_task_create(
  void       *task_func,
  const char *name,
  uint32_t    stack_depth,
  void       *param,
  uint32_t    prio,
  void       *task_handle,
  uint32_t    core_id
)
{
  rtems_status_code sc;
  rtems_id          id;
  rtems_task_priority rtems_prio;

  (void) core_id;

  if ( bt_task_start_count >= (int) RTEMS_ARRAY_SIZE( bt_task_starts ) ) {
    printk( "rtems-esp-bt: out of task slots for '%s'\n", name );
    return 0;
  }

  /*
   * FreeRTOS counts priority upwards, RTEMS downwards, so the mapping has to
   * invert rather than scale.  ESP-IDF runs the controller task at
   * configMAX_PRIORITIES - 2, which is 23 of 25 -- near the top, and well
   * above whatever the application is doing.
   *
   * Getting this backwards does not fail loudly.  With the controller below
   * the task waiting on it, btdm_controller_enable() simply never returns.
   *
   * Applications using this have to leave room: the init task defaults to
   * RTEMS priority 1, which is higher than anything here can be.
   */
  rtems_prio = prio >= 20 ? 4 : 30;

  sc = rtems_task_create(
    rtems_build_name( 'B', 'T', name[ 0 ], name[ 1 ] ),
    rtems_prio,
    stack_depth < 4096 ? 4096 : stack_depth,
    RTEMS_DEFAULT_MODES,
    RTEMS_FLOATING_POINT | RTEMS_LOCAL,
    &id
  );

  if ( sc != RTEMS_SUCCESSFUL ) {
    printk( "rtems-esp-bt: task_create('%s', stack %u) failed (%d)\n",
            name, (unsigned) stack_depth, sc );
    return 0;
  }

  bt_task_starts[ bt_task_start_count ].entry = task_func;
  bt_task_starts[ bt_task_start_count ].arg = param;

  sc = rtems_task_start( id, bt_task_trampoline,
                         (rtems_task_argument) bt_task_start_count );

  if ( sc != RTEMS_SUCCESSFUL ) {
    printk( "rtems-esp-bt: task_start('%s') failed (%d)\n", name, sc );
    rtems_task_delete( id );
    return 0;
  }

  ++bt_task_start_count;

  printk( "rtems-esp-bt: task '%s' created, stack %u, prio %u -> %u\n",
          name, (unsigned) stack_depth, (unsigned) prio,
          (unsigned) rtems_prio );

  if ( task_handle != NULL ) {
    *(rtems_id *) task_handle = id;
  }

  return 1;
}

static void bt_task_delete( void *task_handle )
{
  if ( task_handle == NULL ) {
    rtems_task_exit();
  }

  rtems_task_delete( *(rtems_id *) task_handle );
}

static bool bt_is_in_isr( void )
{
  rtems_esp_bt_calls |= BT_CALL_IS_IN_ISR;
  return rtems_interrupt_is_in_progress();
}

static void bt_task_yield( void )
{
  rtems_task_wake_after( RTEMS_YIELD_PROCESSOR );
}

static void bt_task_yield_from_isr( void )
{
  /* Nothing: RTEMS reschedules on return from the interrupt. */
}

static uint32_t bt_saved_level;

static void bt_global_intr_disable( void )
{
  rtems_interrupt_level level;

  rtems_interrupt_local_disable( level );
  bt_saved_level = level;
}

static void bt_global_intr_restore( void )
{
  rtems_interrupt_level level = bt_saved_level;

  rtems_interrupt_local_enable( level );
}

/* -------------------------------------------------------------------- */
/* Interrupts                                                           */
/* -------------------------------------------------------------------- */

typedef struct {
  bool                used;
  rtems_vector_number vector;
} bt_isr_t;

static bt_isr_t bt_isrs[ 8 ];

static int bt_interrupt_alloc(
  int   cpu_id,
  int   source,
  void ( *handler )( void * ),
  void *arg,
  void **ret_handle
)
{
  rtems_status_code sc;

  rtems_esp_bt_calls |= BT_CALL_ISR_ALLOC;
  (void) cpu_id;

  for ( int i = 0; i < (int) RTEMS_ARRAY_SIZE( bt_isrs ); ++i ) {
    if ( bt_isrs[ i ].used ) {
      continue;
    }

    sc = rtems_interrupt_handler_install(
      (rtems_vector_number) source,
      "ble",
      RTEMS_INTERRUPT_SHARED,
      handler,
      arg
    );

    if ( sc != RTEMS_SUCCESSFUL ) {
      printk( "rtems-esp-bt: handler_install(source %d) failed (%d)\n",
              source, sc );
      return -1;
    }

    bt_isrs[ i ].used = true;
    bt_isrs[ i ].vector = (rtems_vector_number) source;

    if ( ret_handle != NULL ) {
      *ret_handle = &bt_isrs[ i ];
    }

    printk( "rtems-esp-bt: interrupt source %d installed\n", source );

    return 0;
  }

  printk( "rtems-esp-bt: out of ISR slots\n" );

  return -1;
}

static int bt_interrupt_free( void *handle )
{
  bt_isr_t *isr = handle;

  if ( isr == NULL ) {
    return -1;
  }

  isr->used = false;

  return 0;
}

static int bt_interrupt_enable( void *handle )
{
  bt_isr_t *isr = handle;

  return rtems_interrupt_vector_enable( isr->vector ) == RTEMS_SUCCESSFUL
         ? 0 : -1;
}

static int bt_interrupt_disable( void *handle )
{
  bt_isr_t *isr = handle;

  return rtems_interrupt_vector_disable( isr->vector ) == RTEMS_SUCCESSFUL
         ? 0 : -1;
}

static void bt_interrupt_handler_set_rsv(
  int   interrupt_no,
  void ( *fn )( void * ),
  void *arg
)
{
  (void) fn;
  (void) arg;

  printk( "rtems-esp-bt: _interrupt_handler_set_rsv(%d) reached\n",
          interrupt_no );
}

static int bt_cause_sw_intr_to_core( int core_id, int intr_no )
{
  rtems_esp_bt_calls |= BT_CALL_SW_INTR;
  (void) core_id;

  return rtems_interrupt_raise( (rtems_vector_number) intr_no )
         == RTEMS_SUCCESSFUL ? 0 : -1;
}

/* -------------------------------------------------------------------- */
/* Memory, time, entropy                                                */
/* -------------------------------------------------------------------- */

static void *bt_malloc( size_t size )
{
  return malloc( size );
}

static void bt_free( void *p )
{
  free( p );
}

static int bt_read_efuse_mac( uint8_t mac[ 6 ] )
{
  uint32_t mac0 = REG( EFUSE_MAC0_REG );
  uint32_t mac1 = REG( EFUSE_MAC1_REG );

  /* mac[0] is the most significant byte, opposite to the two eFuse words. */
  mac[ 0 ] = (uint8_t) ( ( mac1 >> 8 ) & 0xffu );
  mac[ 1 ] = (uint8_t) ( mac1 & 0xffu );
  mac[ 2 ] = (uint8_t) ( ( mac0 >> 24 ) & 0xffu );
  mac[ 3 ] = (uint8_t) ( ( mac0 >> 16 ) & 0xffu );
  mac[ 4 ] = (uint8_t) ( ( mac0 >> 8 ) & 0xffu );
  mac[ 5 ] = (uint8_t) ( mac0 & 0xffu );

  return 0;
}

static void bt_srand( unsigned int seed )
{
  (void) seed;
}

static int bt_rand( void )
{
  rtems_esp_bt_calls |= BT_CALL_RAND;
  /*
   * The hardware RNG.  Real entropy here because the RF subsystem is on by
   * the time the controller asks: this is what generates resolvable private
   * addresses and the access address of every connection.
   */
  return (int) REG( 0x6001C114u );
}

static int64_t bt_get_time_us( void )
{
  return (int64_t) ( rtems_clock_get_uptime_nanoseconds() / 1000u );
}

static void bt_delay_us( uint32_t us )
{
  rtems_counter_ticks start = rtems_counter_read();
  rtems_counter_ticks want = rtems_counter_nanoseconds_to_ticks( us * 1000u );

  while ( rtems_counter_read() - start < want ) {
    /* wait */
  }
}

static void bt_assert( void )
{
  printk( "rtems-esp-bt: controller assert\n" );
  rtems_fatal( RTEMS_FATAL_SOURCE_APPLICATION, 0xb7 );
}

/* -------------------------------------------------------------------- */
/* Stubs: sleep and coexistence                                         */
/* -------------------------------------------------------------------- */

static bool bt_stub_reported[ 12 ];

static void bt_stub( int which, const char *name )
{
  if ( !bt_stub_reported[ which ] ) {
    bt_stub_reported[ which ] = true;
    printk( "rtems-esp-bt: stub %s reached\n", name );
  }
}

static uint32_t bt_lpcycles_2_hus( uint32_t cycles, uint32_t *error_corr )
{
  bt_stub( 0, "_btdm_lpcycles_2_hus" );
  if ( error_corr != NULL ) {
    *error_corr = 0;
  }
  return cycles;
}

static uint32_t bt_hus_2_lpcycles( uint32_t hus )
{
  bt_stub( 1, "_btdm_hus_2_lpcycles" );
  return hus;
}

static bool bt_sleep_check_duration( int32_t *slot_cnt )
{
  (void) slot_cnt;
  bt_stub( 2, "_btdm_sleep_check_duration" );
  return false;
}

static void bt_sleep_enter_phase1( uint32_t lpcycles )
{
  (void) lpcycles;
  bt_stub( 3, "_btdm_sleep_enter_phase1" );
}

static void bt_sleep_noarg_1( void ) { bt_stub( 4, "_btdm_sleep_enter_phase2" ); }
static void bt_sleep_noarg_2( void ) { bt_stub( 5, "_btdm_sleep_exit_phase1" ); }
static void bt_sleep_noarg_3( void ) { bt_stub( 6, "_btdm_sleep_exit_phase2" ); }
static void bt_sleep_noarg_4( void ) { bt_stub( 7, "_btdm_sleep_exit_phase3" ); }

static void bt_coex_wifi_sleep_set( bool sleep ) { (void) sleep; }
static int  bt_coex_ble_prio_get( bool *low, bool *high )
{
  if ( low != NULL ) { *low = false; }
  if ( high != NULL ) { *high = false; }
  return 0;
}
static int  bt_coex_register_cb( void *cb ) { (void) cb; return 0; }
static void bt_coex_bit_set( uint32_t t, uint32_t s ) { (void) t; (void) s; }
static void bt_coex_bit_clear( uint32_t t, uint32_t s ) { (void) t; (void) s; }
static uint32_t bt_coex_interval_get( void ) { return 0; }
static uint8_t  bt_coex_period_get( void ) { return 0; }
static void    *bt_coex_phase_get( void ) { return NULL; }
static bool     bt_coex_wakeup_request( void ) { return true; }
static void     bt_coex_wakeup_request_end( void ) { }

static void bt_hw_power_down( void ) { bt_stub( 8, "_esp_hw_power_down" ); }
static void bt_hw_power_up( void ) { bt_stub( 9, "_esp_hw_power_up" ); }
static void bt_backup_dma_copy( uint32_t reg, uint32_t mem, uint32_t n, bool to )
{
  (void) reg; (void) mem; (void) n; (void) to;
  bt_stub( 10, "_ets_backup_dma_copy" );
}
static void *bt_malloc_retention( size_t size )
{
  bt_stub( 11, "_malloc_retention" );
  return malloc( size );
}

static void bt_rom_table_ready( void )
{
  printk( "rtems-esp-bt: controller ROM table ready\n" );
}

/* -------------------------------------------------------------------- */
/* The table                                                            */
/* -------------------------------------------------------------------- */

struct osi_funcs_t {
  uint32_t _magic;
  uint32_t _version;
  int ( *_interrupt_alloc )( int, int, void ( * )( void * ), void *, void ** );
  int ( *_interrupt_free )( void * );
  void ( *_interrupt_handler_set_rsv )( int, void ( * )( void * ), void * );
  void ( *_global_intr_disable )( void );
  void ( *_global_intr_restore )( void );
  void ( *_task_yield )( void );
  void ( *_task_yield_from_isr )( void );
  void *( *_semphr_create )( uint32_t, uint32_t );
  void ( *_semphr_delete )( void * );
  int ( *_semphr_take_from_isr )( void *, void * );
  int ( *_semphr_give_from_isr )( void *, void * );
  int ( *_semphr_take )( void *, uint32_t );
  int ( *_semphr_give )( void * );
  void *( *_mutex_create )( void );
  void ( *_mutex_delete )( void * );
  int ( *_mutex_lock )( void * );
  int ( *_mutex_unlock )( void * );
  void *( *_queue_create )( uint32_t, uint32_t );
  void ( *_queue_delete )( void * );
  int ( *_queue_send )( void *, void *, uint32_t );
  int ( *_queue_send_from_isr )( void *, void *, void * );
  int ( *_queue_recv )( void *, void *, uint32_t );
  int ( *_queue_recv_from_isr )( void *, void *, void * );
  int ( *_task_create )( void *, const char *, uint32_t, void *, uint32_t,
                         void *, uint32_t );
  void ( *_task_delete )( void * );
  bool ( *_is_in_isr )( void );
  int ( *_cause_sw_intr_to_core )( int, int );
  void *( *_malloc )( size_t );
  void *( *_malloc_internal )( size_t );
  void ( *_free )( void * );
  int ( *_read_efuse_mac )( uint8_t[ 6 ] );
  void ( *_srand )( unsigned int );
  int ( *_rand )( void );
  uint32_t ( *_btdm_lpcycles_2_hus )( uint32_t, uint32_t * );
  uint32_t ( *_btdm_hus_2_lpcycles )( uint32_t );
  bool ( *_btdm_sleep_check_duration )( int32_t * );
  void ( *_btdm_sleep_enter_phase1 )( uint32_t );
  void ( *_btdm_sleep_enter_phase2 )( void );
  void ( *_btdm_sleep_exit_phase1 )( void );
  void ( *_btdm_sleep_exit_phase2 )( void );
  void ( *_btdm_sleep_exit_phase3 )( void );
  void ( *_coex_wifi_sleep_set )( bool );
  int ( *_coex_core_ble_conn_dyn_prio_get )( bool *, bool * );
  int ( *_coex_schm_register_btdm_callback )( void * );
  void ( *_coex_schm_status_bit_set )( uint32_t, uint32_t );
  void ( *_coex_schm_status_bit_clear )( uint32_t, uint32_t );
  uint32_t ( *_coex_schm_interval_get )( void );
  uint8_t ( *_coex_schm_curr_period_get )( void );
  void *( *_coex_schm_curr_phase_get )( void );
  int ( *_interrupt_enable )( void * );
  int ( *_interrupt_disable )( void * );
  void ( *_esp_hw_power_down )( void );
  void ( *_esp_hw_power_up )( void );
  void ( *_ets_backup_dma_copy )( uint32_t, uint32_t, uint32_t, bool );
  void *( *_malloc_retention )( size_t );
  void ( *_ets_delay_us )( uint32_t );
  void ( *_btdm_rom_table_ready )( void );
  bool ( *_coex_bt_wakeup_request )( void );
  void ( *_coex_bt_wakeup_request_end )( void );
  int64_t ( *_get_time_us )( void );
  void ( *_assert )( void );
};

static const struct osi_funcs_t bt_osi_funcs = {
  ._magic = OSI_MAGIC_VALUE,
  ._version = OSI_VERSION,
  ._interrupt_alloc = bt_interrupt_alloc,
  ._interrupt_free = bt_interrupt_free,
  ._interrupt_handler_set_rsv = bt_interrupt_handler_set_rsv,
  ._global_intr_disable = bt_global_intr_disable,
  ._global_intr_restore = bt_global_intr_restore,
  ._task_yield = bt_task_yield,
  ._task_yield_from_isr = bt_task_yield_from_isr,
  ._semphr_create = bt_semphr_create,
  ._semphr_delete = bt_semphr_delete,
  ._semphr_take_from_isr = bt_semphr_take_from_isr,
  ._semphr_give_from_isr = bt_semphr_give_from_isr,
  ._semphr_take = bt_semphr_take,
  ._semphr_give = bt_semphr_give,
  ._mutex_create = bt_mutex_create,
  ._mutex_delete = bt_mutex_delete,
  ._mutex_lock = bt_mutex_lock,
  ._mutex_unlock = bt_mutex_unlock,
  ._queue_create = bt_queue_create,
  ._queue_delete = bt_queue_delete,
  ._queue_send = bt_queue_send,
  ._queue_send_from_isr = bt_queue_send_from_isr,
  ._queue_recv = bt_queue_recv,
  ._queue_recv_from_isr = bt_queue_recv_from_isr,
  ._task_create = bt_task_create,
  ._task_delete = bt_task_delete,
  ._is_in_isr = bt_is_in_isr,
  ._cause_sw_intr_to_core = bt_cause_sw_intr_to_core,
  ._malloc = bt_malloc,
  ._malloc_internal = bt_malloc,
  ._free = bt_free,
  ._read_efuse_mac = bt_read_efuse_mac,
  ._srand = bt_srand,
  ._rand = bt_rand,
  ._btdm_lpcycles_2_hus = bt_lpcycles_2_hus,
  ._btdm_hus_2_lpcycles = bt_hus_2_lpcycles,
  ._btdm_sleep_check_duration = bt_sleep_check_duration,
  ._btdm_sleep_enter_phase1 = bt_sleep_enter_phase1,
  ._btdm_sleep_enter_phase2 = bt_sleep_noarg_1,
  ._btdm_sleep_exit_phase1 = bt_sleep_noarg_2,
  ._btdm_sleep_exit_phase2 = bt_sleep_noarg_3,
  ._btdm_sleep_exit_phase3 = bt_sleep_noarg_4,
  ._coex_wifi_sleep_set = bt_coex_wifi_sleep_set,
  ._coex_core_ble_conn_dyn_prio_get = bt_coex_ble_prio_get,
  ._coex_schm_register_btdm_callback = bt_coex_register_cb,
  ._coex_schm_status_bit_set = bt_coex_bit_set,
  ._coex_schm_status_bit_clear = bt_coex_bit_clear,
  ._coex_schm_interval_get = bt_coex_interval_get,
  ._coex_schm_curr_period_get = bt_coex_period_get,
  ._coex_schm_curr_phase_get = bt_coex_phase_get,
  ._interrupt_enable = bt_interrupt_enable,
  ._interrupt_disable = bt_interrupt_disable,
  ._esp_hw_power_down = bt_hw_power_down,
  ._esp_hw_power_up = bt_hw_power_up,
  ._ets_backup_dma_copy = bt_backup_dma_copy,
  ._malloc_retention = bt_malloc_retention,
  ._ets_delay_us = bt_delay_us,
  ._btdm_rom_table_ready = bt_rom_table_ready,
  ._coex_bt_wakeup_request = bt_coex_wakeup_request,
  ._coex_bt_wakeup_request_end = bt_coex_wakeup_request_end,
  ._get_time_us = bt_get_time_us,
  ._assert = bt_assert
};

/* ets_delay_us: the one symbol the blob needs that neither the ROM nor the
 * PHY libraries provide. */
void ets_delay_us( uint32_t us );
void ets_delay_us( uint32_t us )
{
  bt_delay_us( us );
}

/* -------------------------------------------------------------------- */
/* Bring-up                                                             */
/* -------------------------------------------------------------------- */

int rtems_esp_bt_controller_init( void *config );
int rtems_esp_bt_controller_init( void *config )
{
  int result;

  bt_copy_sections();

  printk( "rtems-esp-bt: powering the modem domain...\n" );
  bt_modem_domain_on();

  printk( "rtems-esp-bt: powering the Bluetooth domain...\n" );

  if ( bsp_esp32_bt_enable() != RTEMS_SUCCESSFUL ) {
    printk( "rtems-esp-bt: bsp_esp32_bt_enable failed\n" );
    return -1;
  }

  printk( "rtems-esp-bt: registering the OS adapter (%u entries)...\n",
          (unsigned) ( ( sizeof( bt_osi_funcs ) - 8 ) / sizeof( void * ) ) );

  result = btdm_osi_funcs_register( (void *) &bt_osi_funcs );

  if ( result != 0 ) {
    printk( "rtems-esp-bt: btdm_osi_funcs_register failed (%d)\n", result );
    return -1;
  }

  printk( "rtems-esp-bt: controller version %s\n",
          btdm_controller_get_compile_version() );

  btdm_controller_rom_data_init();

  bt_read_efuse_mac( bt_cal_data.mac );
  printk( "rtems-esp-bt: calibrating for MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
          bt_cal_data.mac[ 0 ], bt_cal_data.mac[ 1 ], bt_cal_data.mac[ 2 ],
          bt_cal_data.mac[ 3 ], bt_cal_data.mac[ 4 ], bt_cal_data.mac[ 5 ] );

  /*
   * The link layer's time base, and the step whose absence looks exactly like
   * a hang.
   *
   * The Riviera Waves scheduler counts in low power cycles; without a source
   * selected it never advances, so btdm_controller_enable() waits for an event
   * the hardware will never raise and no interrupt is ever requested.  The
   * symptom is btdm_controller_init() taking about ten seconds and then
   * btdm_controller_enable() never returning.
   *
   * Main crystal at 40 MHz, divided to 1 MHz, which is ESP-IDF's
   * ESP_BT_SLEEP_CLOCK_MAIN_XTAL path: btdm_lpclk_set_div( xtal_hz / MHZ ).
   * The RTC slow clock is the other option and needs a calibration this port
   * does not do.
   */
  if ( !btdm_lpclk_select_src( 0 ) || !btdm_lpclk_set_div( 40 ) ) {
    printk( "rtems-esp-bt: low power clock setup failed\n" );
    return -1;
  }

  printk( "rtems-esp-bt: low power clock: main XTAL / 40\n" );

  printk( "rtems-esp-bt: btdm_controller_init()...\n" );

  result = btdm_controller_init( config );

  if ( result != 0 ) {
    printk( "rtems-esp-bt: btdm_controller_init failed (%d)\n", result );
    return -1;
  }

  printk( "rtems-esp-bt: controller initialised\n" );

  return 0;
}

volatile uint32_t rtems_esp_bt_calls;

int rtems_esp_bt_controller_enable( void );
int rtems_esp_bt_controller_enable( void )
{

  int result;

  /*
   * The PHY belongs here, not in init.
   *
   * ESP-IDF's esp_bt_controller_init() does not touch the radio at all: it
   * powers the domain, registers the adapter and calls btdm_controller_init().
   * esp_phy_enable( PHY_MODEM_BT ) -- which is register_chipv7_phy() followed
   * by bt_bb_v2_init_cmplx() -- happens in esp_bt_controller_enable(), after
   * the controller has initialised, and the comment there spells the order out.
   *
   * This port had it the other way round and btdm_controller_enable() never
   * returned.
   */
  phy_bbpll_en_usb( true );

  printk( "rtems-esp-bt: register_chipv7_phy( PHY_RF_CAL_FULL )...\n" );

  result = register_chipv7_phy( phy_init_data, &bt_cal_data, PHY_RF_CAL_FULL );

  if ( result == ESP_CAL_DATA_CHECK_FAIL ) {
    printk( "rtems-esp-bt: no saved calibration, so the PHY calibrated fully\n" );
  } else if ( result != 0 ) {
    printk( "rtems-esp-bt: register_chipv7_phy failed (%d)\n", result );
    return -1;
  }

  printk( "rtems-esp-bt: bt_bb_v2_init_cmplx()...\n" );
  bt_bb_v2_init_cmplx( 0 );

  btdm_controller_enable_sleep( false );

  /* ESP-IDF disables PLL track on the C3 and S3 before enabling. */
  sdk_config_extend_set_pll_track( false );

  /*
   * No coex_pti_v2() and no coex_enable().  Both live in libcoexist.a, which
   * arbitrates the antenna between the two radios; with WiFi not running
   * there is nothing to arbitrate, and the library is not linked.  This is
   * the line to revisit the day something wants both radios at once.
   */

  {
    static const struct {
      rtems_vector_number vector;
      void              ( *handler )( void );
      const char         *name;
    } isrs[] = {
      { RWBT_INTR, r_rwbtdm_isr_wrapper, "RWBT" },
      { RWBLE_INTR, r_rwbtdm_isr_wrapper, "RWBLE" },
      { BT_BB_INTR, r_bt_bb_isr, "BT_BB" }
    };

    for ( size_t i = 0; i < RTEMS_ARRAY_SIZE( isrs ); ++i ) {
      rtems_status_code sc = rtems_interrupt_handler_install(
        isrs[ i ].vector,
        isrs[ i ].name,
        RTEMS_INTERRUPT_SHARED,
        (rtems_interrupt_handler) (void *) isrs[ i ].handler,
        NULL
      );

      printk( "rtems-esp-bt: %s ISR on source %u: %s\n", isrs[ i ].name,
              (unsigned) isrs[ i ].vector,
              sc == RTEMS_SUCCESSFUL ? "installed" : "FAILED" );

      if ( sc != RTEMS_SUCCESSFUL ) {
        return -1;
      }
    }
  }

  printk( "rtems-esp-bt: btdm_controller_enable( BLE )...\n" );

  /* ESP_BT_MODE_BLE */
  result = btdm_controller_enable( 1 );

  if ( result != 0 ) {
    printk( "rtems-esp-bt: btdm_controller_enable failed (%d)\n", result );
    return -1;
  }

  printk( "rtems-esp-bt: controller enabled\n" );

  return 0;
}
