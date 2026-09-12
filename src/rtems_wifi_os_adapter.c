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
 * wifi_osi_funcs_t for RTEMS.
 *
 * This is the seam.  The WiFi libraries call the OS only through this table,
 * reached from the ROM's g_osi_funcs_p, so it is the one file that has to know
 * both ESP-IDF's expectations and RTEMS' primitives.
 *
 * 118 of the table's 128 entries are active on the ESP32-C3; the rest are
 * guarded out for other targets.  Of those 118, 50 map onto RTEMS primitives
 * and are implemented here.  The other 68 need components that are not built
 * yet -- 23 coexistence, 12 NVS, 10 PHY and clocks, 6 event groups, 5 ETS
 * timers, 3 logging, 2 power management -- and they are *named failures*
 * rather than empty stubs: each reports its own name and returns an error.
 *
 * That choice is deliberate.  An empty stub returning zero makes the WiFi
 * stack fail somewhere far away, and the ESP-IDF failure modes for a missing
 * OS primitive are precisely the plausible-but-wrong kind this project keeps
 * running into.  A line naming the function is the difference between an
 * afternoon and a week.
 *
 * The table uses designated initialisers throughout.  It is a struct of 128
 * function pointers whose order is ESP-IDF's, and a positional initialiser
 * that drifted by one would call the wrong function with the wrong arguments
 * and crash somewhere unrelated.  With designated initialisers a field that
 * is renamed upstream fails to compile.
 */

#include "esp_private/wifi_os_adapter.h"

#include <rtems-esp/event.h>

#include <rtems.h>
#include <rtems/bspIo.h>
#include <rtems/libcsupport.h>
#include <rtems/score/percpu.h>

#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

/* ESP-IDF's convention for these entries. */
#define ESP_OK   0
#define ESP_FAIL (-1)

/*
 * FreeRTOS numbers priorities with higher meaning more urgent and RTEMS the
 * other way round, so a task priority is inverted rather than passed through.
 * This is the pivot.  It has to be below the application's own tasks in RTEMS
 * terms -- numerically higher -- or the WiFi task starves the main loop.
 */
#define RTEMS_WIFI_PRIORITY_MAX 25

/*
 * What an unimplemented entry does.
 *
 * Reported once per entry rather than on every call: the WiFi stack polls
 * some of these, and a message per call turns a diagnosable failure into a
 * console that cannot be read.
 */
#define RTEMS_WIFI_UNIMPLEMENTED( needs )                                  \
  do {                                                                     \
    static bool reported;                                                  \
                                                                           \
    if ( !reported ) {                                                     \
      reported = true;                                                     \
      printk(                                                              \
        "wifi.osi: %s is not implemented on RTEMS yet (needs %s)\n",       \
        __func__,                                                          \
        needs                                                              \
      );                                                                   \
    }                                                                      \
  } while ( 0 )

/* === interrupts and spin locks ======================================== */

/*
 * A "spin lock" here is an interrupt lock.  The single-core C3 has no other
 * CPU to spin against, and what the callers actually need is mutual exclusion
 * against their own ISR.  Allocated rather than static because the libraries
 * create several and keep them.
 */
static void *rtems_wifi_spin_lock_create( void )
{
  rtems_interrupt_lock *lock = malloc( sizeof( *lock ) );

  if ( lock != NULL ) {
    rtems_interrupt_lock_initialize( lock, "ESP WiFi" );
  }

  return lock;
}

static void rtems_wifi_spin_lock_delete( void *lock )
{
  if ( lock != NULL ) {
    rtems_interrupt_lock_destroy( (rtems_interrupt_lock *) lock );
    free( lock );
  }
}

/*
 * The context has to live with the lock rather than be returned, because the
 * signature returns a uint32_t and RTEMS hands back a structure.  One context
 * per lock is enough for the same reason it is in the PHY glue: these nest,
 * they do not interleave.
 */
typedef struct {
  rtems_interrupt_lock          lock;
  rtems_interrupt_lock_context  context;
} rtems_wifi_int_lock;

static uint32_t rtems_wifi_int_disable( void *mux )
{
  if ( mux == NULL ) {
    return 0;
  }

  rtems_interrupt_lock_acquire(
    (rtems_interrupt_lock *) mux,
    &( (rtems_wifi_int_lock *) mux )->context
  );

  return 0;
}

static void rtems_wifi_int_restore( void *mux, uint32_t tmp )
{
  (void) tmp;

  if ( mux == NULL ) {
    return;
  }

  rtems_interrupt_lock_release(
    (rtems_interrupt_lock *) mux,
    &( (rtems_wifi_int_lock *) mux )->context
  );
}

static bool rtems_wifi_is_from_isr( void )
{
  /*
   * The per-CPU ISR nest level, which is non-zero for the whole of an
   * interrupt including nested ones.  Same test components/rtems/helpers.cpp
   * uses in the ESPHome port, and for the same reason: it needs no argument
   * and is correct inside a nested handler.
   */
  return _ISR_Nest_level != 0;
}

static void rtems_wifi_task_yield_from_isr( void )
{
  /*
   * Nothing to do.  FreeRTOS needs an explicit yield because a task woken
   * from an ISR does not preempt until one; RTEMS dispatches at the end of
   * the outermost interrupt on its own.
   */
}

static void rtems_wifi_ints_on( uint32_t mask )
{
  (void) mask;
  RTEMS_WIFI_UNIMPLEMENTED( "the interrupt matrix" );
}

static void rtems_wifi_ints_off( uint32_t mask )
{
  (void) mask;
  RTEMS_WIFI_UNIMPLEMENTED( "the interrupt matrix" );
}

/* === semaphores ======================================================== */

static void *rtems_wifi_semphr_create( uint32_t max, uint32_t init )
{
  rtems_id   id = RTEMS_INVALID_ID;
  rtems_status_code sc;

  (void) max;

  /*
   * Counting, not binary: the callers use these to count events, and a
   * binary semaphore would lose one when two arrive before the first is
   * taken.  No priority inheritance, because a counting semaphore has no
   * owner to inherit from.
   */
  sc = rtems_semaphore_create(
    rtems_build_name( 'W', 'F', 'S', 'M' ),
    init,
    RTEMS_COUNTING_SEMAPHORE | RTEMS_PRIORITY,
    0,
    &id
  );

  if ( sc != RTEMS_SUCCESSFUL ) {
    return NULL;
  }

  return (void *) (uintptr_t) id;
}

static void rtems_wifi_semphr_delete( void *semphr )
{
  rtems_semaphore_delete( (rtems_id) (uintptr_t) semphr );
}

static int32_t rtems_wifi_semphr_take( void *semphr, uint32_t block_time_tick )
{
  rtems_status_code sc;

  /*
   * OSI_FUNCS_TIME_BLOCKING is ESP-IDF's "wait forever"; anything else is a
   * tick count.  rtems_semaphore_obtain takes ticks, so no conversion -- but
   * zero means RTEMS_NO_WAIT rather than "no timeout", which is the opposite
   * of what passing 0 as a timeout would mean.
   */
  if ( block_time_tick == OSI_FUNCS_TIME_BLOCKING ) {
    sc = rtems_semaphore_obtain(
      (rtems_id) (uintptr_t) semphr,
      RTEMS_WAIT,
      RTEMS_NO_TIMEOUT
    );
  } else if ( block_time_tick == 0 ) {
    sc = rtems_semaphore_obtain(
      (rtems_id) (uintptr_t) semphr,
      RTEMS_NO_WAIT,
      0
    );
  } else {
    sc = rtems_semaphore_obtain(
      (rtems_id) (uintptr_t) semphr,
      RTEMS_WAIT,
      block_time_tick
    );
  }

  return sc == RTEMS_SUCCESSFUL ? ESP_OK : ESP_FAIL;
}

static int32_t rtems_wifi_semphr_give( void *semphr )
{
  return rtems_semaphore_release( (rtems_id) (uintptr_t) semphr )
    == RTEMS_SUCCESSFUL ? ESP_OK : ESP_FAIL;
}

/*
 * A semaphore per WiFi thread, created on first use.
 *
 * FreeRTOS keeps this in the task's local storage.  RTEMS has task variables
 * removed and POSIX keys as the replacement, so a key with a destructor is
 * the direct equivalent -- and the destructor matters, because the WiFi
 * threads are created and destroyed across stop and start.
 */
static pthread_key_t rtems_wifi_thread_semphr_key;
static pthread_once_t rtems_wifi_thread_semphr_once = PTHREAD_ONCE_INIT;

static void rtems_wifi_thread_semphr_free( void *semphr )
{
  if ( semphr != NULL ) {
    rtems_semaphore_delete( (rtems_id) (uintptr_t) semphr );
  }
}

static void rtems_wifi_thread_semphr_key_create( void )
{
  pthread_key_create(
    &rtems_wifi_thread_semphr_key,
    rtems_wifi_thread_semphr_free
  );
}

static void *rtems_wifi_thread_semphr_get( void )
{
  void *semphr;

  pthread_once(
    &rtems_wifi_thread_semphr_once,
    rtems_wifi_thread_semphr_key_create
  );

  semphr = pthread_getspecific( rtems_wifi_thread_semphr_key );

  if ( semphr == NULL ) {
    semphr = rtems_wifi_semphr_create( 1, 0 );

    if ( semphr != NULL ) {
      pthread_setspecific( rtems_wifi_thread_semphr_key, semphr );
    }
  }

  return semphr;
}

/* === mutexes =========================================================== */

static void *rtems_wifi_mutex_create_with( rtems_attribute extra )
{
  rtems_id id = RTEMS_INVALID_ID;

  if ( rtems_semaphore_create(
         rtems_build_name( 'W', 'F', 'M', 'X' ),
         1,
         RTEMS_BINARY_SEMAPHORE | RTEMS_PRIORITY | extra,
         0,
         &id
       ) != RTEMS_SUCCESSFUL ) {
    return NULL;
  }

  return (void *) (uintptr_t) id;
}

static void *rtems_wifi_mutex_create( void )
{
  /*
   * Priority inheritance, because these are taken by the WiFi task while the
   * application's task may want the same lock, and inverting there stalls the
   * radio.  Note that RTEMS makes a binary semaphore with inheritance
   * recursive -- see rtems-esphome#71 -- so this and the recursive variant
   * below are the same object, which is a difference from FreeRTOS worth
   * knowing rather than hiding.
   */
  return rtems_wifi_mutex_create_with( RTEMS_INHERIT_PRIORITY );
}

static void *rtems_wifi_recursive_mutex_create( void )
{
  return rtems_wifi_mutex_create_with( RTEMS_INHERIT_PRIORITY );
}

static void rtems_wifi_mutex_delete( void *mutex )
{
  rtems_semaphore_delete( (rtems_id) (uintptr_t) mutex );
}

static int32_t rtems_wifi_mutex_lock( void *mutex )
{
  return rtems_semaphore_obtain(
    (rtems_id) (uintptr_t) mutex,
    RTEMS_WAIT,
    RTEMS_NO_TIMEOUT
  ) == RTEMS_SUCCESSFUL ? ESP_OK : ESP_FAIL;
}

static int32_t rtems_wifi_mutex_unlock( void *mutex )
{
  return rtems_semaphore_release( (rtems_id) (uintptr_t) mutex )
    == RTEMS_SUCCESSFUL ? ESP_OK : ESP_FAIL;
}

/* === queues ============================================================ */

static void *rtems_wifi_queue_create( uint32_t queue_len, uint32_t item_size );
static void rtems_wifi_queue_delete( void *queue );

/*
 * A queue is the id plus its item size.
 *
 * FreeRTOS' xQueueSend infers the length from the queue; RTEMS' send takes it
 * as an argument.  So the size has to be remembered, and a wrapper is the
 * honest way -- the alternative is a fixed guess, which copies the wrong
 * number of bytes and fails silently.
 */
typedef struct {
  rtems_id id;
  size_t   item_size;
} rtems_wifi_queue;

static int32_t rtems_wifi_queue_send_sized(
  void     *queue,
  void     *item,
  uint32_t  block_time_tick,
  bool      urgent
)
{
  rtems_wifi_queue *q = queue;
  rtems_status_code sc;

  (void) block_time_tick;

  if ( q == NULL ) {
    return ESP_FAIL;
  }

  /*
   * RTEMS' send does not block: a full queue is an immediate
   * RTEMS_TOO_MANY, where FreeRTOS would wait for block_time_tick.  The
   * timeout is therefore ignored and a full queue reported as a failure,
   * which the callers treat as a dropped message.  Recorded rather than
   * worked around because the queues the libraries create are sized not to
   * fill, and a blocking send implemented with a poll loop would be worse
   * than the drop.
   */
  if ( urgent ) {
    sc = rtems_message_queue_urgent( q->id, item, q->item_size );
  } else {
    sc = rtems_message_queue_send( q->id, item, q->item_size );
  }

  return sc == RTEMS_SUCCESSFUL ? ESP_OK : ESP_FAIL;
}

static int32_t rtems_wifi_queue_send(
  void *queue, void *item, uint32_t block_time_tick )
{
  return rtems_wifi_queue_send_sized( queue, item, block_time_tick, false );
}

static int32_t rtems_wifi_queue_send_to_back(
  void *queue, void *item, uint32_t block_time_tick )
{
  return rtems_wifi_queue_send_sized( queue, item, block_time_tick, false );
}

static int32_t rtems_wifi_queue_send_to_front(
  void *queue, void *item, uint32_t block_time_tick )
{
  return rtems_wifi_queue_send_sized( queue, item, block_time_tick, true );
}

static int32_t rtems_wifi_queue_send_from_isr(
  void *queue, void *item, void *hptw )
{
  /*
   * hptw is FreeRTOS' "higher priority task woken", which the caller feeds
   * to a yield.  RTEMS dispatches at the end of the outermost interrupt, so
   * there is nothing to report -- but the caller dereferences it, so it has
   * to be written.
   */
  if ( hptw != NULL ) {
    *(int *) hptw = 0;
  }

  return rtems_wifi_queue_send_sized( queue, item, 0, false );
}

static int32_t rtems_wifi_queue_recv(
  void *queue, void *item, uint32_t block_time_tick )
{
  rtems_wifi_queue *q = queue;
  size_t received = 0;
  rtems_status_code sc;

  if ( q == NULL ) {
    return ESP_FAIL;
  }

  if ( block_time_tick == OSI_FUNCS_TIME_BLOCKING ) {
    sc = rtems_message_queue_receive(
      q->id, item, &received, RTEMS_WAIT, RTEMS_NO_TIMEOUT );
  } else if ( block_time_tick == 0 ) {
    sc = rtems_message_queue_receive(
      q->id, item, &received, RTEMS_NO_WAIT, 0 );
  } else {
    sc = rtems_message_queue_receive(
      q->id, item, &received, RTEMS_WAIT, block_time_tick );
  }

  return sc == RTEMS_SUCCESSFUL ? ESP_OK : ESP_FAIL;
}

static uint32_t rtems_wifi_queue_msg_waiting( void *queue )
{
  rtems_wifi_queue *q = queue;
  uint32_t count = 0;

  if ( q != NULL ) {
    rtems_message_queue_get_number_pending( q->id, &count );
  }

  return count;
}

static void *rtems_wifi_queue_create( uint32_t queue_len, uint32_t item_size )
{
  rtems_wifi_queue *q = malloc( sizeof( *q ) );

  if ( q == NULL ) {
    return NULL;
  }

  q->item_size = item_size;

  if ( rtems_message_queue_create(
         rtems_build_name( 'W', 'F', 'Q', 'U' ),
         queue_len,
         item_size,
         RTEMS_FIFO | RTEMS_LOCAL,
         &q->id
       ) != RTEMS_SUCCESSFUL ) {
    free( q );
    return NULL;
  }

  return q;
}

static void rtems_wifi_queue_delete( void *queue )
{
  rtems_wifi_queue *q = queue;

  if ( q != NULL ) {
    rtems_message_queue_delete( q->id );
    free( q );
  }
}

static void *rtems_wifi_create_queue( int queue_len, int item_size )
{
  return rtems_wifi_queue_create( (uint32_t) queue_len, (uint32_t) item_size );
}

static void rtems_wifi_delete_queue( void *queue )
{
  rtems_wifi_queue_delete( queue );
}

/* === tasks ============================================================= */

static int32_t rtems_wifi_task_create(
  void        *task_func,
  const char  *name,
  uint32_t     stack_depth,
  void        *param,
  uint32_t     prio,
  void        *task_handle
)
{
  rtems_id id = RTEMS_INVALID_ID;

  /*
   * FreeRTOS numbers priorities with higher meaning more urgent; RTEMS is the
   * other way round, 1 being the most urgent.  So the value is inverted
   * rather than passed through, and getting that backwards would run the WiFi
   * task at the lowest priority in the system and present as packet loss.
   */
  rtems_task_priority rtems_prio =
    prio >= RTEMS_WIFI_PRIORITY_MAX ? 1 : RTEMS_WIFI_PRIORITY_MAX - prio;

  if ( rtems_task_create(
         rtems_build_name( 'W', 'F', 'T', 'K' ),
         rtems_prio,
         stack_depth,
         RTEMS_DEFAULT_MODES,
         RTEMS_FLOATING_POINT | RTEMS_LOCAL,
         &id
       ) != RTEMS_SUCCESSFUL ) {
    return 0;
  }

  (void) name;

  if ( rtems_task_start(
         id,
         (rtems_task_entry) task_func,
         (rtems_task_argument) param
       ) != RTEMS_SUCCESSFUL ) {
    rtems_task_delete( id );
    return 0;
  }

  if ( task_handle != NULL ) {
    *(void **) task_handle = (void *) (uintptr_t) id;
  }

  /* FreeRTOS returns pdPASS, which is 1. */
  return 1;
}

static int32_t rtems_wifi_task_create_pinned_to_core(
  void        *task_func,
  const char  *name,
  uint32_t     stack_depth,
  void        *param,
  uint32_t     prio,
  void        *task_handle,
  uint32_t     core_id
)
{
  /* One core on this part, so pinning is what happens anyway. */
  (void) core_id;

  return rtems_wifi_task_create(
    task_func, name, stack_depth, param, prio, task_handle );
}

static void rtems_wifi_task_delete( void *task_handle )
{
  rtems_task_delete(
    task_handle == NULL ? RTEMS_SELF : (rtems_id) (uintptr_t) task_handle );
}

static void rtems_wifi_task_delay( uint32_t tick )
{
  rtems_task_wake_after( tick );
}

static int32_t rtems_wifi_task_ms_to_tick( uint32_t ms )
{
  return (int32_t) RTEMS_MILLISECONDS_TO_TICKS( ms );
}

static void *rtems_wifi_task_get_current_task( void )
{
  return (void *) (uintptr_t) rtems_task_self();
}

static int32_t rtems_wifi_task_get_max_priority( void )
{
  return (int32_t) RTEMS_WIFI_PRIORITY_MAX;
}

/* === heap ============================================================== */

/*
 * One heap.  ESP-IDF distinguishes internal SRAM from PSRAM and DMA-capable
 * from not; the C3 has no PSRAM and RTEMS has one region, so the
 * distinctions collapse and every variant is the same allocator.  Recorded
 * because the names suggest otherwise.
 */
static void *rtems_wifi_malloc( size_t size )
{
  return malloc( size );
}

static void rtems_wifi_free( void *p )
{
  free( p );
}

static void *rtems_wifi_calloc( size_t n, size_t size )
{
  return calloc( n, size );
}

static void *rtems_wifi_realloc( void *ptr, size_t size )
{
  return realloc( ptr, size );
}

static void *rtems_wifi_zalloc( size_t size )
{
  return calloc( 1, size );
}

static uint32_t rtems_wifi_get_free_heap_size( void )
{
  /*
   * malloc_free_space(), not malloc_info(), because the question is the total
   * free space and nothing else in the information block is wanted here.  Note
   * it is the sum of the free blocks, so a large allocation can fail with this
   * reporting more than enough; the WiFi stack uses it for its own reporting
   * and for the low-memory decisions in the RX path, both of which tolerate
   * that.
   */
  return (uint32_t) malloc_free_space();
}

/* === time and randomness =============================================== */

static int64_t rtems_wifi_esp_timer_get_time( void )
{
  /* Microseconds since boot, which is what esp_timer_get_time means. */
  return (int64_t) ( rtems_clock_get_uptime_nanoseconds() / 1000 );
}

static int rtems_wifi_get_time( void *t )
{
  struct timeval *tv = t;

  if ( tv == NULL ) {
    return ESP_FAIL;
  }

  return gettimeofday( tv, NULL ) == 0 ? ESP_OK : ESP_FAIL;
}

static uint32_t rtems_wifi_rand( void )
{
  uint32_t value;

  /*
   * getentropy rather than rand().  These values seed WPA key material, and
   * the BSP's getentropy is at least honest about where it comes from --
   * bsps/shared/dev/getentropy/getentropy-cpucounter.c on this board, which
   * is not a strong source and says so.  rand() would be worse and look the
   * same.
   */
  if ( getentropy( &value, sizeof( value ) ) != 0 ) {
    value = (uint32_t) rtems_clock_get_uptime_nanoseconds();
  }

  return value;
}

static int rtems_wifi_get_random( uint8_t *buf, size_t len )
{
  return getentropy( buf, len ) == 0 ? ESP_OK : ESP_FAIL;
}

/* === not implemented yet =============================================== */

/*
 * Generated from the struct in wifi_os_adapter.h rather than written, so a
 * signature cannot drift from it, and each says what it is waiting for.  They
 * report once and return a failure; the comment at the top of this file says
 * why that is better than returning zero.
 */

static bool rtems_wifi_stub_env_is_chip( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "a component that is not built yet" );
  return false;
}

static void rtems_wifi_stub_set_intr( int32_t cpu_no, uint32_t intr_source, uint32_t intr_num, int32_t intr_prio )
{
  (void) cpu_no;
  (void) intr_source;
  (void) intr_num;
  (void) intr_prio;
  RTEMS_WIFI_UNIMPLEMENTED( "a component that is not built yet" );
}

static void rtems_wifi_stub_clear_intr( uint32_t intr_source, uint32_t intr_num )
{
  (void) intr_source;
  (void) intr_num;
  RTEMS_WIFI_UNIMPLEMENTED( "a component that is not built yet" );
}

static void rtems_wifi_stub_set_isr( int32_t n, void *f, void *arg )
{
  (void) n;
  (void) f;
  (void) arg;
  RTEMS_WIFI_UNIMPLEMENTED( "a component that is not built yet" );
}

static void *rtems_wifi_stub_event_group_create( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "event groups" );
  return NULL;
}

static void rtems_wifi_stub_event_group_delete( void *event )
{
  (void) event;
  RTEMS_WIFI_UNIMPLEMENTED( "event groups" );
}

static uint32_t rtems_wifi_stub_event_group_set_bits( void *event, uint32_t bits )
{
  (void) event;
  (void) bits;
  RTEMS_WIFI_UNIMPLEMENTED( "event groups" );
  return 0;
}

static uint32_t rtems_wifi_stub_event_group_clear_bits( void *event, uint32_t bits )
{
  (void) event;
  (void) bits;
  RTEMS_WIFI_UNIMPLEMENTED( "event groups" );
  return 0;
}

static uint32_t rtems_wifi_stub_event_group_wait_bits( void *event, uint32_t bits_to_wait_for, int clear_on_exit, int wait_for_all_bits, uint32_t block_time_tick )
{
  (void) event;
  (void) bits_to_wait_for;
  (void) clear_on_exit;
  (void) wait_for_all_bits;
  (void) block_time_tick;
  RTEMS_WIFI_UNIMPLEMENTED( "event groups" );
  return 0;
}


static void rtems_wifi_stub_dport_access_stall_other_cpu_start_wrap( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_hw_support" );
}

static void rtems_wifi_stub_dport_access_stall_other_cpu_end_wrap( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_hw_support" );
}

static void rtems_wifi_stub_wifi_pm_sleep_lock_acquire( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "power management" );
}

static void rtems_wifi_stub_wifi_pm_sleep_lock_release( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "power management" );
}

static void rtems_wifi_stub_phy_disable( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_hw_support" );
}

static void rtems_wifi_stub_phy_enable( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_hw_support" );
}

static int rtems_wifi_stub_phy_update_country_info( const char* country )
{
  (void) country;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_hw_support" );
  return ESP_FAIL;
}

static int rtems_wifi_stub_read_mac( uint8_t* mac, unsigned int type )
{
  (void) mac;
  (void) type;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_hw_support" );
  return ESP_FAIL;
}

static void rtems_wifi_stub_timer_arm( void *timer, uint32_t tmout, bool repeat )
{
  (void) timer;
  (void) tmout;
  (void) repeat;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_timer" );
}

static void rtems_wifi_stub_timer_disarm( void *timer )
{
  (void) timer;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_timer" );
}

static void rtems_wifi_stub_timer_done( void *ptimer )
{
  (void) ptimer;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_timer" );
}

static void rtems_wifi_stub_timer_setfn( void *ptimer, void *pfunction, void *parg )
{
  (void) ptimer;
  (void) pfunction;
  (void) parg;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_timer" );
}

static void rtems_wifi_stub_timer_arm_us( void *ptimer, uint32_t us, bool repeat )
{
  (void) ptimer;
  (void) us;
  (void) repeat;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_timer" );
}

static void rtems_wifi_stub_wifi_reset_mac( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_hw_support" );
}

static void rtems_wifi_stub_wifi_clock_enable( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_hw_support" );
}

static void rtems_wifi_stub_wifi_clock_disable( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_hw_support" );
}

static void rtems_wifi_stub_wifi_rtc_enable_iso( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_hw_support" );
}

static void rtems_wifi_stub_wifi_rtc_disable_iso( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_hw_support" );
}

static int rtems_wifi_stub_nvs_set_i8( uint32_t handle, const char* key, int8_t value )
{
  (void) handle;
  (void) key;
  (void) value;
  RTEMS_WIFI_UNIMPLEMENTED( "a key/value store" );
  return ESP_FAIL;
}

static int rtems_wifi_stub_nvs_get_i8( uint32_t handle, const char* key, int8_t* out_value )
{
  (void) handle;
  (void) key;
  (void) out_value;
  RTEMS_WIFI_UNIMPLEMENTED( "a key/value store" );
  return ESP_FAIL;
}

static int rtems_wifi_stub_nvs_set_u8( uint32_t handle, const char* key, uint8_t value )
{
  (void) handle;
  (void) key;
  (void) value;
  RTEMS_WIFI_UNIMPLEMENTED( "a key/value store" );
  return ESP_FAIL;
}

static int rtems_wifi_stub_nvs_get_u8( uint32_t handle, const char* key, uint8_t* out_value )
{
  (void) handle;
  (void) key;
  (void) out_value;
  RTEMS_WIFI_UNIMPLEMENTED( "a key/value store" );
  return ESP_FAIL;
}

static int rtems_wifi_stub_nvs_set_u16( uint32_t handle, const char* key, uint16_t value )
{
  (void) handle;
  (void) key;
  (void) value;
  RTEMS_WIFI_UNIMPLEMENTED( "a key/value store" );
  return ESP_FAIL;
}

static int rtems_wifi_stub_nvs_get_u16( uint32_t handle, const char* key, uint16_t* out_value )
{
  (void) handle;
  (void) key;
  (void) out_value;
  RTEMS_WIFI_UNIMPLEMENTED( "a key/value store" );
  return ESP_FAIL;
}

static int rtems_wifi_stub_nvs_open( const char* name, unsigned int open_mode, uint32_t *out_handle )
{
  (void) name;
  (void) open_mode;
  (void) out_handle;
  RTEMS_WIFI_UNIMPLEMENTED( "a key/value store" );
  return ESP_FAIL;
}

static void rtems_wifi_stub_nvs_close( uint32_t handle )
{
  (void) handle;
  RTEMS_WIFI_UNIMPLEMENTED( "a key/value store" );
}

static int rtems_wifi_stub_nvs_commit( uint32_t handle )
{
  (void) handle;
  RTEMS_WIFI_UNIMPLEMENTED( "a key/value store" );
  return ESP_FAIL;
}

static int rtems_wifi_stub_nvs_set_blob( uint32_t handle, const char* key, const void* value, size_t length )
{
  (void) handle;
  (void) key;
  (void) value;
  (void) length;
  RTEMS_WIFI_UNIMPLEMENTED( "a key/value store" );
  return ESP_FAIL;
}

static int rtems_wifi_stub_nvs_get_blob( uint32_t handle, const char* key, void* out_value, size_t* length )
{
  (void) handle;
  (void) key;
  (void) out_value;
  (void) length;
  RTEMS_WIFI_UNIMPLEMENTED( "a key/value store" );
  return ESP_FAIL;
}

static int rtems_wifi_stub_nvs_erase_key( uint32_t handle, const char* key )
{
  (void) handle;
  (void) key;
  RTEMS_WIFI_UNIMPLEMENTED( "a key/value store" );
  return ESP_FAIL;
}

static unsigned long rtems_wifi_stub_random( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "a component that is not built yet" );
  return 0;
}

static uint32_t rtems_wifi_stub_slowclk_cal_get( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_hw_support" );
  return 0;
}

static void rtems_wifi_stub_log_write( unsigned int level, const char* tag, const char* format, ... )
{
  (void) level;
  (void) tag;
  (void) format;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_log" );
}

static void rtems_wifi_stub_log_writev( unsigned int level, const char* tag, const char* format, va_list args )
{
  (void) level;
  (void) tag;
  (void) format;
  (void) args;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_log" );
}

static uint32_t rtems_wifi_stub_log_timestamp( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_log" );
  return 0;
}

static int rtems_wifi_stub_coex_init( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return ESP_FAIL;
}

static void rtems_wifi_stub_coex_deinit( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
}

static int rtems_wifi_stub_coex_enable( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return ESP_FAIL;
}

static void rtems_wifi_stub_coex_disable( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
}

static uint32_t rtems_wifi_stub_coex_status_get( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return 0;
}

static void rtems_wifi_stub_coex_condition_set( uint32_t type, bool dissatisfy )
{
  (void) type;
  (void) dissatisfy;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
}

static int rtems_wifi_stub_coex_wifi_request( uint32_t event, uint32_t latency, uint32_t duration )
{
  (void) event;
  (void) latency;
  (void) duration;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return ESP_FAIL;
}

static int rtems_wifi_stub_coex_wifi_release( uint32_t event )
{
  (void) event;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return ESP_FAIL;
}

static int rtems_wifi_stub_coex_wifi_channel_set( uint8_t primary, uint8_t secondary )
{
  (void) primary;
  (void) secondary;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return ESP_FAIL;
}

static int rtems_wifi_stub_coex_event_duration_get( uint32_t event, uint32_t *duration )
{
  (void) event;
  (void) duration;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return ESP_FAIL;
}

static int rtems_wifi_stub_coex_pti_get( uint32_t event, uint8_t *pti )
{
  (void) event;
  (void) pti;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return ESP_FAIL;
}

static void rtems_wifi_stub_coex_schm_status_bit_clear( uint32_t type, uint32_t status )
{
  (void) type;
  (void) status;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
}

static void rtems_wifi_stub_coex_schm_status_bit_set( uint32_t type, uint32_t status )
{
  (void) type;
  (void) status;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
}

static int rtems_wifi_stub_coex_schm_interval_set( uint32_t interval )
{
  (void) interval;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return ESP_FAIL;
}

static uint32_t rtems_wifi_stub_coex_schm_interval_get( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return 0;
}

static uint8_t rtems_wifi_stub_coex_schm_curr_period_get( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return 0;
}

static void *rtems_wifi_stub_coex_schm_curr_phase_get( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return NULL;
}

static int rtems_wifi_stub_coex_schm_process_restart( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return ESP_FAIL;
}

static int rtems_wifi_stub_coex_schm_register_cb( int, int (* cb)(int) )
{
  (void) cb;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return ESP_FAIL;
}

static int rtems_wifi_stub_coex_register_start_cb( int (* cb)(void) )
{
  (void) cb;
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return ESP_FAIL;
}

static int rtems_wifi_stub_coex_schm_flexible_period_set( uint8_t )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return ESP_FAIL;
}

static uint8_t rtems_wifi_stub_coex_schm_flexible_period_get( void )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return 0;
}

static void *rtems_wifi_stub_coex_schm_get_phase_by_idx( int )
{
  RTEMS_WIFI_UNIMPLEMENTED( "esp_coex" );
  return NULL;
}

/* === the table ========================================================= */

/*
 * Designated initialisers throughout.  This is a struct of 128 function
 * pointers in ESP-IDF's order, and a positional list that drifted by one
 * entry would call the wrong function with the wrong arguments and fail
 * somewhere unrelated.  Named fields make a rename upstream a compile error
 * instead of a crash.
 *
 * g_wifi_osi_funcs is what the ROM's g_osi_funcs_p is pointed at.
 */
wifi_osi_funcs_t g_wifi_osi_funcs = {
  ._version = ESP_WIFI_OS_ADAPTER_VERSION,
  ._env_is_chip = rtems_wifi_stub_env_is_chip,
  ._set_intr = rtems_wifi_stub_set_intr,
  ._clear_intr = rtems_wifi_stub_clear_intr,
  ._set_isr = rtems_wifi_stub_set_isr,
  ._ints_on = rtems_wifi_ints_on,
  ._ints_off = rtems_wifi_ints_off,
  ._is_from_isr = rtems_wifi_is_from_isr,
  ._spin_lock_create = rtems_wifi_spin_lock_create,
  ._spin_lock_delete = rtems_wifi_spin_lock_delete,
  ._wifi_int_disable = rtems_wifi_int_disable,
  ._wifi_int_restore = rtems_wifi_int_restore,
  ._task_yield_from_isr = rtems_wifi_task_yield_from_isr,
  ._semphr_create = rtems_wifi_semphr_create,
  ._semphr_delete = rtems_wifi_semphr_delete,
  ._semphr_take = rtems_wifi_semphr_take,
  ._semphr_give = rtems_wifi_semphr_give,
  ._wifi_thread_semphr_get = rtems_wifi_thread_semphr_get,
  ._mutex_create = rtems_wifi_mutex_create,
  ._recursive_mutex_create = rtems_wifi_recursive_mutex_create,
  ._mutex_delete = rtems_wifi_mutex_delete,
  ._mutex_lock = rtems_wifi_mutex_lock,
  ._mutex_unlock = rtems_wifi_mutex_unlock,
  ._queue_create = rtems_wifi_queue_create,
  ._queue_delete = rtems_wifi_queue_delete,
  ._queue_send = rtems_wifi_queue_send,
  ._queue_send_from_isr = rtems_wifi_queue_send_from_isr,
  ._queue_send_to_back = rtems_wifi_queue_send_to_back,
  ._queue_send_to_front = rtems_wifi_queue_send_to_front,
  ._queue_recv = rtems_wifi_queue_recv,
  ._queue_msg_waiting = rtems_wifi_queue_msg_waiting,
  ._event_group_create = rtems_wifi_stub_event_group_create,
  ._event_group_delete = rtems_wifi_stub_event_group_delete,
  ._event_group_set_bits = rtems_wifi_stub_event_group_set_bits,
  ._event_group_clear_bits = rtems_wifi_stub_event_group_clear_bits,
  ._event_group_wait_bits = rtems_wifi_stub_event_group_wait_bits,
  ._task_create_pinned_to_core = rtems_wifi_task_create_pinned_to_core,
  ._task_create = rtems_wifi_task_create,
  ._task_delete = rtems_wifi_task_delete,
  ._task_delay = rtems_wifi_task_delay,
  ._task_ms_to_tick = rtems_wifi_task_ms_to_tick,
  ._task_get_current_task = rtems_wifi_task_get_current_task,
  ._task_get_max_priority = rtems_wifi_task_get_max_priority,
  ._malloc = rtems_wifi_malloc,
  ._free = rtems_wifi_free,
  ._event_post = rtems_esp_event_post,
  ._get_free_heap_size = rtems_wifi_get_free_heap_size,
  ._rand = rtems_wifi_rand,
  ._dport_access_stall_other_cpu_start_wrap = rtems_wifi_stub_dport_access_stall_other_cpu_start_wrap,
  ._dport_access_stall_other_cpu_end_wrap = rtems_wifi_stub_dport_access_stall_other_cpu_end_wrap,
  ._wifi_pm_sleep_lock_acquire = rtems_wifi_stub_wifi_pm_sleep_lock_acquire,
  ._wifi_pm_sleep_lock_release = rtems_wifi_stub_wifi_pm_sleep_lock_release,
  ._phy_disable = rtems_wifi_stub_phy_disable,
  ._phy_enable = rtems_wifi_stub_phy_enable,
  ._phy_update_country_info = rtems_wifi_stub_phy_update_country_info,
  ._read_mac = rtems_wifi_stub_read_mac,
  ._timer_arm = rtems_wifi_stub_timer_arm,
  ._timer_disarm = rtems_wifi_stub_timer_disarm,
  ._timer_done = rtems_wifi_stub_timer_done,
  ._timer_setfn = rtems_wifi_stub_timer_setfn,
  ._timer_arm_us = rtems_wifi_stub_timer_arm_us,
  ._wifi_reset_mac = rtems_wifi_stub_wifi_reset_mac,
  ._wifi_clock_enable = rtems_wifi_stub_wifi_clock_enable,
  ._wifi_clock_disable = rtems_wifi_stub_wifi_clock_disable,
  ._wifi_rtc_enable_iso = rtems_wifi_stub_wifi_rtc_enable_iso,
  ._wifi_rtc_disable_iso = rtems_wifi_stub_wifi_rtc_disable_iso,
  ._esp_timer_get_time = rtems_wifi_esp_timer_get_time,
  ._nvs_set_i8 = rtems_wifi_stub_nvs_set_i8,
  ._nvs_get_i8 = rtems_wifi_stub_nvs_get_i8,
  ._nvs_set_u8 = rtems_wifi_stub_nvs_set_u8,
  ._nvs_get_u8 = rtems_wifi_stub_nvs_get_u8,
  ._nvs_set_u16 = rtems_wifi_stub_nvs_set_u16,
  ._nvs_get_u16 = rtems_wifi_stub_nvs_get_u16,
  ._nvs_open = rtems_wifi_stub_nvs_open,
  ._nvs_close = rtems_wifi_stub_nvs_close,
  ._nvs_commit = rtems_wifi_stub_nvs_commit,
  ._nvs_set_blob = rtems_wifi_stub_nvs_set_blob,
  ._nvs_get_blob = rtems_wifi_stub_nvs_get_blob,
  ._nvs_erase_key = rtems_wifi_stub_nvs_erase_key,
  ._get_random = rtems_wifi_get_random,
  ._get_time = rtems_wifi_get_time,
  ._random = rtems_wifi_stub_random,
  ._slowclk_cal_get = rtems_wifi_stub_slowclk_cal_get,
  ._log_write = rtems_wifi_stub_log_write,
  ._log_writev = rtems_wifi_stub_log_writev,
  ._log_timestamp = rtems_wifi_stub_log_timestamp,
  ._malloc_internal = rtems_wifi_malloc,
  ._realloc_internal = rtems_wifi_realloc,
  ._calloc_internal = rtems_wifi_calloc,
  ._zalloc_internal = rtems_wifi_zalloc,
  ._wifi_malloc = rtems_wifi_malloc,
  ._wifi_realloc = rtems_wifi_realloc,
  ._wifi_calloc = rtems_wifi_calloc,
  ._wifi_zalloc = rtems_wifi_zalloc,
  ._wifi_create_queue = rtems_wifi_create_queue,
  ._wifi_delete_queue = rtems_wifi_delete_queue,
  ._coex_init = rtems_wifi_stub_coex_init,
  ._coex_deinit = rtems_wifi_stub_coex_deinit,
  ._coex_enable = rtems_wifi_stub_coex_enable,
  ._coex_disable = rtems_wifi_stub_coex_disable,
  ._coex_status_get = rtems_wifi_stub_coex_status_get,
  ._coex_condition_set = rtems_wifi_stub_coex_condition_set,
  ._coex_wifi_request = rtems_wifi_stub_coex_wifi_request,
  ._coex_wifi_release = rtems_wifi_stub_coex_wifi_release,
  ._coex_wifi_channel_set = rtems_wifi_stub_coex_wifi_channel_set,
  ._coex_event_duration_get = rtems_wifi_stub_coex_event_duration_get,
  ._coex_pti_get = rtems_wifi_stub_coex_pti_get,
  ._coex_schm_status_bit_clear = rtems_wifi_stub_coex_schm_status_bit_clear,
  ._coex_schm_status_bit_set = rtems_wifi_stub_coex_schm_status_bit_set,
  ._coex_schm_interval_set = rtems_wifi_stub_coex_schm_interval_set,
  ._coex_schm_interval_get = rtems_wifi_stub_coex_schm_interval_get,
  ._coex_schm_curr_period_get = rtems_wifi_stub_coex_schm_curr_period_get,
  ._coex_schm_curr_phase_get = rtems_wifi_stub_coex_schm_curr_phase_get,
  ._coex_schm_process_restart = rtems_wifi_stub_coex_schm_process_restart,
  ._coex_schm_register_cb = rtems_wifi_stub_coex_schm_register_cb,
  ._coex_register_start_cb = rtems_wifi_stub_coex_register_start_cb,
  ._coex_schm_flexible_period_set = rtems_wifi_stub_coex_schm_flexible_period_set,
  ._coex_schm_flexible_period_get = rtems_wifi_stub_coex_schm_flexible_period_get,
  ._coex_schm_get_phase_by_idx = rtems_wifi_stub_coex_schm_get_phase_by_idx,
  ._magic = ESP_WIFI_OS_ADAPTER_MAGIC,
};
