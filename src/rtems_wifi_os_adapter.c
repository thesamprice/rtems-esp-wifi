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
 * guarded out for other targets.  Of those 118, 59 map onto RTEMS primitives
 * and are implemented here -- including the five event groups and the five ETS
 * timers, which have no RTEMS equivalent and are built out of pthreads and the
 * timer server respectively.  The other 59 need components that are not built
 * yet -- 23 coexistence, 12 NVS, 12 PHY, clocks and MAC, 7 interrupt plumbing,
 * 3 logging, 2 power management -- and they are *named failures* rather than
 * empty stubs: each reports its own name and returns an error.
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
#include <bsp/irq-generic.h>
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
 * pdTRUE and pdFALSE, not ESP_OK and ESP_FAIL.
 *
 * The entries that ESP-IDF implements with xQueueSend, xQueueReceive,
 * xSemaphoreTake and friends return FreeRTOS status, where success is 1.  Half
 * of this table is that shape and half is esp_err_t, where success is 0, and
 * the two are exact opposites.
 *
 * Getting it backwards is not a crash.  esp_wifi_init_internal() returned
 * ESP_ERR_WIFI_POST -- "failed to post the event to WiFi task" -- because
 * every successful send reported failure.  A queue that works perfectly and
 * claims to have failed is a good deal harder to find than one that does not
 * work, so the two constants are named here rather than written as 0 and 1 at
 * each site.
 */
#define RTEMS_WIFI_PD_TRUE  1
#define RTEMS_WIFI_PD_FALSE 0


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

  return sc == RTEMS_SUCCESSFUL ? RTEMS_WIFI_PD_TRUE : RTEMS_WIFI_PD_FALSE;
}

static int32_t rtems_wifi_semphr_give( void *semphr )
{
  return rtems_semaphore_release( (rtems_id) (uintptr_t) semphr )
    == RTEMS_SUCCESSFUL ? RTEMS_WIFI_PD_TRUE : RTEMS_WIFI_PD_FALSE;
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
  ) == RTEMS_SUCCESSFUL ? RTEMS_WIFI_PD_TRUE : RTEMS_WIFI_PD_FALSE;
}

static int32_t rtems_wifi_mutex_unlock( void *mutex )
{
  return rtems_semaphore_release( (rtems_id) (uintptr_t) mutex )
    == RTEMS_SUCCESSFUL ? RTEMS_WIFI_PD_TRUE : RTEMS_WIFI_PD_FALSE;
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
    return RTEMS_WIFI_PD_FALSE;
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

  return sc == RTEMS_SUCCESSFUL ? RTEMS_WIFI_PD_TRUE : RTEMS_WIFI_PD_FALSE;
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
    return RTEMS_WIFI_PD_FALSE;
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

  return sc == RTEMS_SUCCESSFUL ? RTEMS_WIFI_PD_TRUE : RTEMS_WIFI_PD_FALSE;
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

/*
 * _wifi_create_queue does NOT return a queue handle, and returning one is a
 * null dereference in the WiFi task about a second after the libraries start.
 *
 * ESP-IDF's wifi_create_queue() returns a wifi_static_queue_t (esp_private/
 * wifi.h): { QueueHandle_t handle; void *storage; }.  The libraries keep that
 * pointer, read ->handle out of it, and pass *the handle* to _queue_send and
 * _queue_recv -- not the thing this function returned.
 *
 * Returning the rtems_wifi_queue directly therefore half-works: the blob reads
 * its first word, which is the rtems_id, and hands that back as the queue.
 * _queue_recv then dereferences an object id as a pointer.  It faulted at
 * mcause 0x5 with a5 = 0x22010002, which is an id rather than an address, and
 * that value is the only reason the cause was findable.
 *
 * storage stays null: it exists so FreeRTOS can be handed statically allocated
 * queue memory, and RTEMS' message queues own theirs.
 */
typedef struct {
  void *handle;
  void *storage;
} rtems_wifi_static_queue;

static void *rtems_wifi_create_queue( int queue_len, int item_size )
{
  rtems_wifi_static_queue *sq = malloc( sizeof( *sq ) );

  if ( sq == NULL ) {
    return NULL;
  }

  sq->handle  = rtems_wifi_queue_create( (uint32_t) queue_len,
                                         (uint32_t) item_size );
  sq->storage = NULL;

  if ( sq->handle == NULL ) {
    free( sq );

    return NULL;
  }

  return sq;
}

static void rtems_wifi_delete_queue( void *queue )
{
  rtems_wifi_static_queue *sq = queue;

  if ( sq == NULL ) {
    return;
  }

  /* The wrapper this points at, then the wifi_static_queue_t itself. */
  rtems_wifi_queue_delete( sq->handle );
  free( sq );
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
  void *p = malloc( size );

#ifdef RTEMS_WIFI_DEBUG_ALLOC
  /*
   * A failed allocation here is silent and costly.  ppInstallKey() asks for
   * key_len + 168 bytes through _wifi_malloc before it programs a key, and on
   * NULL it returns 0x101 and installs nothing -- which presents as an
   * association that completes, a four-way handshake that completes, and no
   * encrypted frame passing in either direction, with no error anywhere.
   */
  if ( p == NULL ) {
    printk( "wifi.osi: malloc(%u) FAILED\n", (unsigned) size );
  }
#endif

  return p;
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

/* === event groups ====================================================== */

/*
 * A FreeRTOS event group is one shared bit mask with several waiters, each
 * blocked on its own combination of bits and its own "all of" or "any of"
 * rule.  RTEMS' rtems_event_send/receive look like the same thing and are
 * not: their bits belong to one task, so a set that has to wake three
 * waiters has nowhere to go.  A mutex and a condition variable is the
 * construct that actually matches, and pthreads are already in use here for
 * the per-thread semaphore key.
 *
 * Neither set_bits nor clear_bits may be called from an ISR: they take a
 * pthread mutex.  ESP-IDF has a separate xEventGroupSetBitsFromISR for that
 * case and this table has no entry for it, so no caller should need to.
 */
typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t  cond;
  uint32_t        bits;
} rtems_wifi_event_group;

static void *rtems_wifi_event_group_create( void )
{
  rtems_wifi_event_group *g = malloc( sizeof( *g ) );
  pthread_condattr_t      attr;

  if ( g == NULL ) {
    return NULL;
  }

  g->bits = 0;
  pthread_mutex_init( &g->mutex, NULL );

  /*
   * CLOCK_MONOTONIC for the wait deadlines.  The default is CLOCK_REALTIME,
   * which this port moves when SNTP sets the time, and a step backwards there
   * would turn a 100 ms wait into a long one.
   */
  pthread_condattr_init( &attr );
  pthread_condattr_setclock( &attr, CLOCK_MONOTONIC );
  pthread_cond_init( &g->cond, &attr );
  pthread_condattr_destroy( &attr );

  return g;
}

static void rtems_wifi_event_group_delete( void *event )
{
  rtems_wifi_event_group *g = event;

  if ( g == NULL ) {
    return;
  }

  /*
   * FreeRTOS' vEventGroupDelete unblocks the waiters first.  There is no way
   * to do that here and then free the memory they are about to touch, so a
   * group with waiters must not be deleted; the callers delete these while
   * tearing down, after the tasks that wait on them are gone.
   */
  pthread_cond_destroy( &g->cond );
  pthread_mutex_destroy( &g->mutex );
  free( g );
}

static uint32_t rtems_wifi_event_group_set_bits( void *event, uint32_t bits )
{
  rtems_wifi_event_group *g = event;
  uint32_t                value;

  if ( g == NULL ) {
    return 0;
  }

  pthread_mutex_lock( &g->mutex );
  g->bits |= bits;
  value = g->bits;

  /*
   * Broadcast, not signal: the waiters are each waiting for a different
   * combination, so waking only one can wake the one this set did not
   * satisfy and leave a satisfied waiter asleep.
   */
  pthread_cond_broadcast( &g->cond );
  pthread_mutex_unlock( &g->mutex );

  /*
   * The value as of the set.  FreeRTOS returns it as of the *return*, by
   * which point a waiter woken with clear-on-exit may already have taken its
   * bits back out.  Nothing in the WiFi libraries reads this return value,
   * and reporting the mask that was actually set is the more useful of the
   * two if something ever does.
   */
  return value;
}

static uint32_t rtems_wifi_event_group_clear_bits( void *event, uint32_t bits )
{
  rtems_wifi_event_group *g = event;
  uint32_t                value;

  if ( g == NULL ) {
    return 0;
  }

  /* The value *before* the clear, which is what xEventGroupClearBits
   * returns.  No broadcast: clearing bits cannot satisfy a waiter. */
  pthread_mutex_lock( &g->mutex );
  value = g->bits;
  g->bits &= ~bits;
  pthread_mutex_unlock( &g->mutex );

  return value;
}

static uint32_t rtems_wifi_event_group_wait_bits(
  void     *event,
  uint32_t  bits_to_wait_for,
  int       clear_on_exit,
  int       wait_for_all_bits,
  uint32_t  block_time_tick
)
{
  rtems_wifi_event_group *g = event;
  struct timespec         deadline;
  uint32_t                value;
  bool                    satisfied;
  bool                    timed_out = false;

  if ( g == NULL ) {
    return 0;
  }

  /*
   * Waiting for no bits can never be satisfied by the "any of" rule and is
   * always satisfied by "all of".  FreeRTOS asserts on it; returning the
   * current mask is the one answer that cannot hang.
   */
  if ( bits_to_wait_for == 0 ) {
    return rtems_wifi_event_group_clear_bits( event, 0 );
  }

  if ( block_time_tick != OSI_FUNCS_TIME_BLOCKING && block_time_tick != 0 ) {
    uint64_t ns = (uint64_t) block_time_tick
      * rtems_configuration_get_microseconds_per_tick() * 1000;

    clock_gettime( CLOCK_MONOTONIC, &deadline );
    deadline.tv_sec += (time_t) ( ns / 1000000000ULL );
    deadline.tv_nsec += (long) ( ns % 1000000000ULL );

    if ( deadline.tv_nsec >= 1000000000L ) {
      deadline.tv_nsec -= 1000000000L;
      deadline.tv_sec += 1;
    }
  }

  pthread_mutex_lock( &g->mutex );

  for ( ;; ) {
    value = g->bits;

    /*
     * "All of" is every requested bit present, "any of" is at least one.  The
     * returned value is the mask as it was when the wait was satisfied, before
     * clear-on-exit takes the bits out again -- a caller that is told only the
     * cleared mask cannot tell which of several bits woke it.
     */
    if ( wait_for_all_bits ) {
      satisfied = ( value & bits_to_wait_for ) == bits_to_wait_for;
    } else {
      satisfied = ( value & bits_to_wait_for ) != 0;
    }

    if ( satisfied ) {
      if ( clear_on_exit ) {
        g->bits &= ~bits_to_wait_for;
      }

      break;
    }

    /*
     * Zero is a poll rather than "no timeout", the same inversion the
     * semaphore take has, and a wait that has already timed out still gets
     * one last look at the mask above before it gives up.
     */
    if ( block_time_tick == 0 || timed_out ) {
      break;
    }

    if ( block_time_tick == OSI_FUNCS_TIME_BLOCKING ) {
      pthread_cond_wait( &g->cond, &g->mutex );
    } else {
      timed_out =
        pthread_cond_timedwait( &g->cond, &g->mutex, &deadline ) == ETIMEDOUT;
    }
  }

  pthread_mutex_unlock( &g->mutex );

  return value;
}

/* === ETS timers ======================================================== */

/*
 * ETS timers are the ROM's timers and the caller owns the storage: it passes
 * a pointer to its own ETSTimer, five 32-bit words, and expects setfn to
 * initialise it and done to release it.  So the RTEMS state has to live
 * inside those five words rather than beside them, and the magic is what
 * tells an initialised one from whatever the caller's memory held before.
 */
#define RTEMS_WIFI_ETS_TIMER_MAGIC 0x5746

typedef struct {
  rtems_id   id;
  void     (*func)( void *arg );
  void      *arg;
  uint32_t   interval;
  uint16_t   magic;
  bool       repeat;
} rtems_wifi_ets_timer;

RTEMS_STATIC_ASSERT(
  sizeof( rtems_wifi_ets_timer ) <= 5 * sizeof( uint32_t ),
  ets_timer_fits_in_the_callers_ETSTimer
);

/*
 * ESP-IDF dispatches these from the esp_timer task at FreeRTOS priority 22,
 * one below the WiFi task's 23, and the callbacks rely on being in a task:
 * they take mutexes and post to queues.  So the RTEMS timer *server* runs
 * them, not the clock tick's interrupt context, and its priority is inverted
 * the same way rtems_wifi_task_create inverts -- higher FreeRTOS number,
 * lower RTEMS number.
 */
#define RTEMS_WIFI_ETS_TIMER_PRIORITY ( RTEMS_WIFI_PRIORITY_MAX - 22 )

static pthread_once_t rtems_wifi_timer_server_once = PTHREAD_ONCE_INIT;

static void rtems_wifi_timer_server_start( void )
{
  rtems_status_code sc = rtems_timer_initiate_server(
    RTEMS_WIFI_ETS_TIMER_PRIORITY,
    RTEMS_MINIMUM_STACK_SIZE * 4,
    RTEMS_DEFAULT_ATTRIBUTES
  );

  /*
   * RTEMS_INCORRECT_STATE means the application started it already, which is
   * fine -- there is one server and it is shared.  Anything else is a
   * configuration problem (no task or no timer left) that would otherwise
   * present as a WiFi stack whose timeouts never fire.
   */
  if ( sc != RTEMS_SUCCESSFUL && sc != RTEMS_INCORRECT_STATE ) {
    printk(
      "wifi.osi: the timer server would not start (%s)\n",
      rtems_status_text( sc )
    );
  }
}

/*
 * Microseconds to ticks, rounded *up* and never to zero.  A sub-tick interval
 * is the case that matters: rounding down gives 0, which rtems_timer_*_after
 * rejects, and a periodic timer that re-armed with 0 would either stop or
 * fire every tick forever.  So the floor is one tick -- with the usual 10 ms
 * tick, the ROM's 640 us minimum becomes 10 ms.  That is a real loss of
 * resolution and the reason a short WiFi timeout may be late, but late is
 * recoverable and a timer that fires continuously is not.
 */
static uint32_t rtems_wifi_us_to_ticks( uint64_t us )
{
  uint64_t per_tick = rtems_configuration_get_microseconds_per_tick();
  uint64_t ticks = ( us + per_tick - 1 ) / per_tick;

  if ( ticks == 0 ) {
    ticks = 1;
  } else if ( ticks > 0xffffffffU ) {
    ticks = 0xffffffffU;
  }

  return (uint32_t) ticks;
}

static void rtems_wifi_timer_tsr( rtems_id id, void *arg )
{
  rtems_wifi_ets_timer *t = arg;

  (void) id;

  if ( t == NULL || t->magic != RTEMS_WIFI_ETS_TIMER_MAGIC ) {
    return;
  }

  /*
   * Re-armed before the callback rather than after, so that a callback which
   * disarms or frees its own timer -- which the WiFi libraries do -- wins
   * instead of being undone by a re-arm behind it.
   */
  if ( t->repeat ) {
    rtems_timer_server_fire_after(
      t->id, t->interval, rtems_wifi_timer_tsr, t );
  }

  if ( t->func != NULL ) {
    t->func( t->arg );
  }
}

static void rtems_wifi_timer_setfn(
  void *ptimer, void *pfunction, void *parg )
{
  rtems_wifi_ets_timer *t = ptimer;

  if ( t == NULL ) {
    return;
  }

  /*
   * setfn is the constructor, and it is also called again on a timer that is
   * already live to change the callback.  The magic is what separates the two:
   * creating a second RTEMS timer for the same ETSTimer would leak the first
   * and leave it firing.
   */
  if ( t->magic != RTEMS_WIFI_ETS_TIMER_MAGIC ) {
    rtems_id id = RTEMS_INVALID_ID;

    pthread_once( &rtems_wifi_timer_server_once, rtems_wifi_timer_server_start );

    if ( rtems_timer_create(
           rtems_build_name( 'W', 'F', 'T', 'M' ),
           &id
         ) != RTEMS_SUCCESSFUL ) {
      printk( "wifi.osi: out of RTEMS timers, CONFIGURE_MAXIMUM_TIMERS\n" );
      return;
    }

    t->id = id;
    t->interval = 1;
    t->repeat = false;
    t->magic = RTEMS_WIFI_ETS_TIMER_MAGIC;
  } else {
    rtems_timer_cancel( t->id );
    t->repeat = false;
  }

  t->func = pfunction;
  t->arg = parg;
}

static void rtems_wifi_timer_arm_ticks(
  rtems_wifi_ets_timer *t, uint32_t ticks, bool repeat )
{
  if ( t == NULL || t->magic != RTEMS_WIFI_ETS_TIMER_MAGIC ) {
    /* Armed before setfn, so there is no timer and no callback to run. */
    printk( "wifi.osi: an ETS timer was armed before _timer_setfn\n" );
    return;
  }

  t->interval = ticks;
  t->repeat = repeat;

  rtems_timer_server_fire_after( t->id, ticks, rtems_wifi_timer_tsr, t );
}

static void rtems_wifi_timer_arm( void *timer, uint32_t tmout, bool repeat )
{
  /* ets_timer_arm takes milliseconds; only arm_us takes microseconds. */
  rtems_wifi_timer_arm_ticks(
    timer, rtems_wifi_us_to_ticks( (uint64_t) tmout * 1000 ), repeat );
}

static void rtems_wifi_timer_arm_us( void *ptimer, uint32_t us, bool repeat )
{
  rtems_wifi_timer_arm_ticks( ptimer, rtems_wifi_us_to_ticks( us ), repeat );
}

static void rtems_wifi_timer_disarm( void *timer )
{
  rtems_wifi_ets_timer *t = timer;

  if ( t == NULL || t->magic != RTEMS_WIFI_ETS_TIMER_MAGIC ) {
    return;
  }

  /*
   * repeat cleared as well as cancelled: a periodic timer whose callback is
   * running on the server right now has already re-armed itself, and
   * cancelling alone would let the next period through.
   */
  t->repeat = false;
  rtems_timer_cancel( t->id );
}

static void rtems_wifi_timer_done( void *ptimer )
{
  rtems_wifi_ets_timer *t = ptimer;

  if ( t == NULL || t->magic != RTEMS_WIFI_ETS_TIMER_MAGIC ) {
    return;
  }

  /* The destructor.  The magic goes first so that a use after this point is
   * an ignored call rather than a stale rtems_id. */
  t->magic = 0;
  t->repeat = false;
  rtems_timer_cancel( t->id );
  rtems_timer_delete( t->id );
  t->func = NULL;
  t->arg = NULL;
}

/* === interrupts ========================================================= */

/*
 * ESP-IDF splits this across three calls with two different numbering
 * schemes, and the split is the only hard part.
 *
 *   _set_intr( cpu, intr_source, intr_num, prio )  routes a PERIPHERAL SOURCE
 *                                                  to a CPU interrupt CHANNEL
 *   _set_isr ( intr_num, handler, arg )            attaches to the CHANNEL
 *   _ints_on ( mask )                              a bitmask of CHANNELS
 *
 * RTEMS does not expose the channel.  This BSP's vector number *is* the
 * peripheral source -- bsp_interrupt_facility_initialize() enables every
 * CPU-side channel up front and does all real routing inside
 * bsp_interrupt_vector_enable(), so a vector and a source are the same number
 * and the channel is the BSP's business.
 *
 * That leaves one thing to do here: remember which source each channel was
 * given, so that _set_isr and _ints_on -- which only ever quote the channel --
 * can be turned back into a vector.  Hence the table.
 */
#define RTEMS_WIFI_INTR_CHANNELS 32

static uint32_t rtems_wifi_intr_source[ RTEMS_WIFI_INTR_CHANNELS ];
static bool     rtems_wifi_intr_mapped[ RTEMS_WIFI_INTR_CHANNELS ];

static void rtems_wifi_set_intr(
  int32_t  cpu_no,
  uint32_t intr_source,
  uint32_t intr_num,
  int32_t  intr_prio
)
{
  rtems_status_code sc;

  /* Single core.  ESP-IDF passes 0 here on the C3 and the argument exists for
   * the dual-core parts. */
  (void) cpu_no;

  if ( intr_num >= RTEMS_WIFI_INTR_CHANNELS ) {
    printk( "wifi.osi: interrupt channel %u is out of range\n",
            (unsigned) intr_num );

    return;
  }

  rtems_wifi_intr_source[ intr_num ] = intr_source;
  rtems_wifi_intr_mapped[ intr_num ] = true;

  /*
   * ESP-IDF's priorities rise with urgency and so do this controller's, so
   * unlike task priorities these pass through unchanged.  Said explicitly
   * because the inversion two hundred lines up makes the opposite look like
   * the house rule.
   */
  sc = bsp_interrupt_set_priority( (rtems_vector_number) intr_source,
                                   (uint32_t) intr_prio );

  if ( sc != RTEMS_SUCCESSFUL ) {
    printk( "wifi.osi: cannot set priority on source %u (%i)\n",
            (unsigned) intr_source, (int) sc );
  }
}

static void rtems_wifi_clear_intr( uint32_t intr_source, uint32_t intr_num )
{
  (void) intr_source;

  if ( intr_num < RTEMS_WIFI_INTR_CHANNELS ) {
    rtems_wifi_intr_mapped[ intr_num ] = false;
  }
}

/*
 * The libraries' handler takes a single void * argument, which is not the
 * shape rtems_interrupt_handler_install() wants; the types are otherwise the
 * same.  Stored per channel rather than passed, because the RTEMS handler
 * argument is already spoken for by the channel number.
 */
typedef void ( *rtems_wifi_isr )( void *arg );

static rtems_wifi_isr rtems_wifi_intr_handler[ RTEMS_WIFI_INTR_CHANNELS ];
static void          *rtems_wifi_intr_arg[ RTEMS_WIFI_INTR_CHANNELS ];

static void rtems_wifi_intr_trampoline( void *arg )
{
  uintptr_t channel = (uintptr_t) arg;

  if ( channel < RTEMS_WIFI_INTR_CHANNELS
       && rtems_wifi_intr_handler[ channel ] != NULL ) {
    ( *rtems_wifi_intr_handler[ channel ] )( rtems_wifi_intr_arg[ channel ] );
  }
}

static void rtems_wifi_set_isr( int32_t n, void *f, void *arg )
{
  rtems_status_code sc;
  uint32_t          channel = (uint32_t) n;

  if ( channel >= RTEMS_WIFI_INTR_CHANNELS
       || !rtems_wifi_intr_mapped[ channel ] ) {
    printk( "wifi.osi: _set_isr on channel %u before _set_intr mapped it\n",
            (unsigned) channel );

    return;
  }

  rtems_wifi_intr_handler[ channel ] = (rtems_wifi_isr) f;
  rtems_wifi_intr_arg[ channel ]     = arg;

  sc = rtems_interrupt_handler_install(
    (rtems_vector_number) rtems_wifi_intr_source[ channel ],
    "wifi",
    RTEMS_INTERRUPT_SHARED,
    rtems_wifi_intr_trampoline,
    (void *) (uintptr_t) channel
  );

  if ( sc != RTEMS_SUCCESSFUL && sc != RTEMS_TOO_MANY ) {
    /*
     * Expected today for the WiFi MAC, and the reason is in the BSP rather
     * than here.  ETS_WIFI_MAC_INTR_SOURCE is 0, and the esp32c3db BSP treats
     * vector 0 as invalid -- bsp_interrupt_is_valid_vector() rejects it and
     * get_active_interrupt() returns 0 to mean "nothing found", so source 0
     * cannot be told apart from no interrupt at all.  The BSP's irq_mappings[]
     * has no WiFi entry either.
     *
     * Reported once per install rather than suppressed: until that is fixed no
     * MAC interrupt can be delivered, so nothing will ever be received, and a
     * silent failure here would look like a radio problem.
     */
    printk( "wifi.osi: cannot install the handler for source %u (%i)\n",
            (unsigned) rtems_wifi_intr_source[ channel ], (int) sc );
  }
}

/*
 * A mask of channels, not of sources, so each bit has to go back through the
 * table.  A bit with no mapping is ignored rather than reported: the libraries
 * enable a mask covering channels they may not have claimed.
 */
static void rtems_wifi_ints_on( uint32_t mask )
{
  uint32_t i;

  for ( i = 0; i < RTEMS_WIFI_INTR_CHANNELS; ++i ) {
    if ( ( mask & ( 1u << i ) ) != 0 && rtems_wifi_intr_mapped[ i ] ) {
      (void) bsp_interrupt_vector_enable(
        (rtems_vector_number) rtems_wifi_intr_source[ i ] );
    }
  }
}

static void rtems_wifi_ints_off( uint32_t mask )
{
  uint32_t i;

  for ( i = 0; i < RTEMS_WIFI_INTR_CHANNELS; ++i ) {
    if ( ( mask & ( 1u << i ) ) != 0 && rtems_wifi_intr_mapped[ i ] ) {
      (void) bsp_interrupt_vector_disable(
        (rtems_vector_number) rtems_wifi_intr_source[ i ] );
    }
  }
}

/* === the chip itself ==================================================== */

/*
 * These were named failures until the libraries were run far enough to call
 * them.  That is what the named-failure stubs are for: rather than guessing
 * which of the 68 the station path needs, esp_wifi_init() and esp_wifi_start()
 * printed the list.  Every one implemented here appeared in that output.
 *
 * They are register work that ESP-IDF puts in esp_hw_support, which this port
 * does not build.  The registers come from the C3's own headers in
 * esp-hal-3rdparty rather than from memory; each is named below.
 */

/* soc/esp32c3/register/soc/reg_base.h and syscon_reg.h */
#define RTEMS_WIFI_SYSCON_BASE        0x60026000u
#define RTEMS_WIFI_CLK_EN_REG         ( RTEMS_WIFI_SYSCON_BASE + 0x014u )
#define RTEMS_WIFI_RST_EN_REG         ( RTEMS_WIFI_SYSCON_BASE + 0x018u )
#define RTEMS_WIFI_WIFIMAC_RST        ( 1u << 2 )   /* SYSTEM_WIFIMAC_RST */

/* soc/esp32c3/register/soc/efuse_reg.h */
#define RTEMS_WIFI_EFUSE_BASE         0x60008800u
#define RTEMS_WIFI_EFUSE_MAC0_REG     ( RTEMS_WIFI_EFUSE_BASE + 0x44u )
#define RTEMS_WIFI_EFUSE_MAC1_REG     ( RTEMS_WIFI_EFUSE_BASE + 0x48u )

static inline uint32_t rtems_wifi_reg_read( uint32_t addr )
{
  return *(volatile uint32_t *) (uintptr_t) addr;
}

static inline void rtems_wifi_reg_write( uint32_t addr, uint32_t value )
{
  *(volatile uint32_t *) (uintptr_t) addr = value;
}

static bool rtems_wifi_env_is_chip( void )
{
  /*
   * True means real silicon.  ESP-IDF returns false only under
   * CONFIG_IDF_ENV_FPGA, for pre-production bring-up on an FPGA where the
   * clocks differ; libphy uses it to pick timing constants.  Anything running
   * this port is a real part -- QEMU included, since it models the part and
   * not the FPGA.
   */
  return true;
}

static void rtems_wifi_clock_enable( void )
{
  /*
   * SYSTEM_WIFI_CLK_WIFI_EN_M is genuinely zero on the ESP32-C3.
   * clk_gate_ll.h defines it as (V << S) with V = 0x0, so ESP-IDF's
   * periph_ll_wifi_module_enable_clk_clear_rst() sets no bits either -- the
   * WiFi clock is not separately gated on this part.  The read-modify-write is
   * kept so the shape matches the other chips and so this does not read as an
   * oversight.
   */
  rtems_wifi_reg_write(
    RTEMS_WIFI_CLK_EN_REG,
    rtems_wifi_reg_read( RTEMS_WIFI_CLK_EN_REG ) | 0u
  );
}

static void rtems_wifi_clock_disable( void )
{
  rtems_wifi_reg_write(
    RTEMS_WIFI_CLK_EN_REG,
    rtems_wifi_reg_read( RTEMS_WIFI_CLK_EN_REG ) & ~0u
  );
}

static void rtems_wifi_reset_mac( void )
{
  uint32_t reg = rtems_wifi_reg_read( RTEMS_WIFI_RST_EN_REG );

  /* periph_module_reset(PERIPH_WIFI_MODULE): assert then release. */
  rtems_wifi_reg_write( RTEMS_WIFI_RST_EN_REG, reg | RTEMS_WIFI_WIFIMAC_RST );
  rtems_wifi_reg_write( RTEMS_WIFI_RST_EN_REG, reg & ~RTEMS_WIFI_WIFIMAC_RST );
}

/* In libphy; declared here because esp_private/phy.h wants _lock_t. */
extern void phy_wakeup_init( void );
extern void phy_close_rf( void );
extern void phy_wifi_enable_set( uint8_t enable );

static void rtems_wifi_phy_enable( void )
{
  phy_wakeup_init();
  phy_wifi_enable_set( 1 );
}

static void rtems_wifi_phy_disable( void )
{
  phy_wifi_enable_set( 0 );
  phy_close_rf();
}

static int rtems_wifi_read_mac( uint8_t *mac, unsigned int type )
{
  uint32_t mac0;
  uint32_t mac1;

  if ( mac == NULL ) {
    return ESP_FAIL;
  }

  mac0 = rtems_wifi_reg_read( RTEMS_WIFI_EFUSE_MAC0_REG );
  mac1 = rtems_wifi_reg_read( RTEMS_WIFI_EFUSE_MAC1_REG );

  /*
   * mac[0] is the MOST significant byte, which is the opposite of how the two
   * eFuse words are laid out -- MAC_1 holds the high 16 bits and MAC_0 the low
   * 32.  esp_hw_support/mac_addr.c states the order where it reassembles them
   * for the CRC check: mac_high = mac[0] << 8 | mac[1].
   *
   * Getting this backwards yields a valid-looking address that no access point
   * will ever answer, so it is written out byte by byte rather than memcpy'd.
   */
  mac[ 0 ] = (uint8_t) ( ( mac1 >> 8 ) & 0xffu );
  mac[ 1 ] = (uint8_t) ( mac1 & 0xffu );
  mac[ 2 ] = (uint8_t) ( ( mac0 >> 24 ) & 0xffu );
  mac[ 3 ] = (uint8_t) ( ( mac0 >> 16 ) & 0xffu );
  mac[ 4 ] = (uint8_t) ( ( mac0 >> 8 ) & 0xffu );
  mac[ 5 ] = (uint8_t) ( mac0 & 0xffu );

  /*
   * Type 0 is the station address, 1 the soft-AP's.  ESP-IDF derives the AP
   * address from the base rather than storing a second one; the universe
   * offset is 1.  Anything else is a radio this port does not have.
   */
  if ( type == 1 ) {
    mac[ 5 ] += 1;
  } else if ( type != 0 ) {
    RTEMS_WIFI_UNIMPLEMENTED( "a MAC type other than station or soft-AP" );

    return ESP_FAIL;
  }

  return ESP_OK;
}

/* === coexistence, which there is nothing to coexist with ================ */

/*
 * The ESP32-C3 has one radio shared between WiFi and Bluetooth, and these
 * entries are how the WiFi side asks for it.  This port builds no Bluetooth,
 * so WiFi always has the radio and the honest implementation is to say yes.
 *
 * Deliberately not named failures.  A named failure is right where the answer
 * is unknown; here it is known, and reporting "not implemented" every time the
 * libraries check for a peer that cannot exist would be noise that hides the
 * entries that do still matter.
 */
static int rtems_wifi_coex_init( void )
{
  return ESP_OK;
}

static void rtems_wifi_coex_deinit( void )
{
}

static int rtems_wifi_coex_enable( void )
{
  return ESP_OK;
}

static void rtems_wifi_coex_disable( void )
{
}

static int rtems_wifi_coex_register_start_cb( int ( *cb )( void ) )
{
  (void) cb;

  /*
   * The callback is how coexistence would tell WiFi it may start a slot.  With
   * no competing radio there is nothing to schedule around, so it is never
   * called and reporting success is accurate rather than a shortcut.
   */
  return ESP_OK;
}

static int rtems_wifi_coex_schm_register_cb( int type, int ( *cb )( int ) )
{
  (void) type;
  (void) cb;

  return ESP_OK;
}

/* === power management, which is switched off =========================== */

/*
 * ESP-IDF's are already empty unless CONFIG_PM_ENABLE, and this port does not
 * build power management at all -- see the file comment on esp_wifi_init().
 * So an empty body is what ESP-IDF compiles too, not a stub standing in for
 * one.
 */
static void rtems_wifi_pm_sleep_lock_acquire( void )
{
}

static void rtems_wifi_pm_sleep_lock_release( void )
{
}

/* === the last few the libraries asked for =============================== */

/*
 * The remaining coexistence entries esp_wifi_start() reaches.  Like the six
 * above they answer rather than report, for the same reason: this port builds
 * no Bluetooth, so WiFi always owns the radio and the answers are known.
 */
static uint32_t rtems_wifi_coex_status_get( void )
{
  return 0;
}

static int rtems_wifi_coex_pti_get( uint32_t service, uint8_t *pti )
{
  (void) service;

  /*
   * Packet traffic arbitration priority.  With no competing radio every
   * request wins, and zero is the value ESP-IDF's own stub returns when
   * coexistence is compiled out.
   */
  if ( pti != NULL ) {
    *pti = 0;
  }

  return 0;
}

static int rtems_wifi_coex_schm_interval_set( uint32_t interval )
{
  (void) interval;

  return ESP_OK;
}

static void rtems_wifi_coex_schm_status_bit_set( uint32_t type, uint32_t status )
{
  (void) type;
  (void) status;
}

static void rtems_wifi_coex_schm_status_bit_clear( uint32_t type, uint32_t status )
{
  (void) type;
  (void) status;
}

/*
 * The RTC slow clock's measured frequency, as a period in 1/2^19 microsecond
 * units -- the format rtc_clk_cal() returns and the WiFi libraries expect for
 * their sleep arithmetic.
 *
 * Returned from the nominal 150 kHz rather than measured.  A real calibration
 * counts slow-clock ticks against the crystal, which needs the RTC and TIMG
 * registers this port does not otherwise touch; the error is the oscillator's
 * own tolerance, which is several percent and matters only for sleep timing
 * that is switched off here anyway.  Worth revisiting with #101 if sleep is
 * ever enabled.
 */
static uint32_t rtems_wifi_slowclk_cal_get( void )
{
  return (uint32_t) ( ( 1000000ULL << 19 ) / 150000ULL );
}

static int rtems_wifi_phy_update_country_info( const char *country )
{
  (void) country;

  /*
   * Regulatory domain changes to the PHY.  The channel and power limits the
   * libraries enforce come from esp_wifi_set_country_code(), which is in
   * libnet80211 and works; this entry is the PHY's own per-country table,
   * which libphy applies internally on the C3.  ESP-IDF's wrapper calls
   * esp_phy_update_country_info() from esp_phy, which this port does not
   * build.
   *
   * Empty rather than a named failure: it is reached on every start, and a
   * report would be noise.  The consequence is that the PHY keeps its default
   * regulatory table.  That is a real limitation and belongs in the release
   * notes rather than in a log line.
   */

  return ESP_OK;
}

/*
 * Stalling the other CPU during DPORT access.  The ESP32-C3 is single core,
 * so there is no other CPU and nothing to stall -- ESP-IDF maps both of these
 * to an empty wrapper on this target too.  Empty rather than a named failure
 * because "not implemented" would be actively misleading: there is nothing to
 * implement.
 */
static void rtems_wifi_dport_stall_start( void )
{
}

static void rtems_wifi_dport_stall_end( void )
{
}

/* unsigned long, not uint32_t: the table says so, and they are the same width
 * here only by coincidence of the ABI. */
static unsigned long rtems_wifi_random( void )
{
  return (unsigned long) rtems_wifi_rand();
}

/* === not implemented yet =============================================== */

/*
 * Generated from the struct in wifi_os_adapter.h rather than written, so a
 * signature cannot drift from it, and each says what it is waiting for.  They
 * report once and return a failure; the comment at the top of this file says
 * why that is better than returning zero.
 */

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

/*
 * The libraries' own logging, which used to be discarded.
 *
 * These three were stubs, and a stub here is worse than it looks.  Everything
 * libnet80211, libpp and the supplicant have to say about why something did
 * not work -- an association rejected, a handshake that timed out, a frame
 * dropped for a reason they know and we do not -- went to
 * RTEMS_WIFI_UNIMPLEMENTED and was lost, so a port that is being brought up
 * against real hardware had its single best diagnostic switched off.
 *
 * printk rather than printf: this is called from the WiFi task and from
 * contexts with interrupts disabled, where printf's locking is not safe, and
 * the whole point is to survive the failure being reported.
 *
 * Filtered, and the filter is not cosmetic.  ESP_LOG_NONE is 0 and the levels
 * rise to ESP_LOG_VERBOSE at 5; the libraries emit a great deal at INFO and
 * below, and printk goes out of a USB-Serial-JTAG console synchronously.
 * Printing all of it during association breaks the association: the run
 * disconnects with reason 4, WIFI_REASON_ASSOC_EXPIRE, because the handshake
 * misses its timing while the console drains.  Errors and warnings are rare
 * enough not to, and they are the ones worth having.
 *
 * Raise it with -DRTEMS_WIFI_LOG_LEVEL=<n> when chasing something specific,
 * and expect the timing cost above at INFO and beyond.
 */
#ifndef RTEMS_WIFI_LOG_LEVEL
#define RTEMS_WIFI_LOG_LEVEL 2
#endif
static void rtems_wifi_log_writev(
  unsigned int level,
  const char  *tag,
  const char  *format,
  va_list      args
)
{
  if ( level > RTEMS_WIFI_LOG_LEVEL ) {
    return;
  }

  if ( tag != NULL ) {
    printk( "wifi.%s: ", tag );
  }

  vprintk( format, args );
}

static void rtems_wifi_log_write(
  unsigned int level,
  const char  *tag,
  const char  *format,
  ...
)
{
  va_list args;

  va_start( args, format );
  rtems_wifi_log_writev( level, tag, format, args );
  va_end( args );
}

/*
 * Milliseconds since boot, which is what esp_log_timestamp() returns and what
 * the libraries' own format strings expect.
 */
static uint32_t rtems_wifi_log_timestamp( void )
{
  return (uint32_t)
    ( rtems_clock_get_uptime_nanoseconds() / 1000000u );
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
  ._env_is_chip = rtems_wifi_env_is_chip,
  ._set_intr = rtems_wifi_set_intr,
  ._clear_intr = rtems_wifi_clear_intr,
  ._set_isr = rtems_wifi_set_isr,
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
  ._event_group_create = rtems_wifi_event_group_create,
  ._event_group_delete = rtems_wifi_event_group_delete,
  ._event_group_set_bits = rtems_wifi_event_group_set_bits,
  ._event_group_clear_bits = rtems_wifi_event_group_clear_bits,
  ._event_group_wait_bits = rtems_wifi_event_group_wait_bits,
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
  ._dport_access_stall_other_cpu_start_wrap = rtems_wifi_dport_stall_start,
  ._dport_access_stall_other_cpu_end_wrap = rtems_wifi_dport_stall_end,
  ._wifi_pm_sleep_lock_acquire = rtems_wifi_pm_sleep_lock_acquire,
  ._wifi_pm_sleep_lock_release = rtems_wifi_pm_sleep_lock_release,
  ._phy_disable = rtems_wifi_phy_disable,
  ._phy_enable = rtems_wifi_phy_enable,
  ._phy_update_country_info = rtems_wifi_phy_update_country_info,
  ._read_mac = rtems_wifi_read_mac,
  ._timer_arm = rtems_wifi_timer_arm,
  ._timer_disarm = rtems_wifi_timer_disarm,
  ._timer_done = rtems_wifi_timer_done,
  ._timer_setfn = rtems_wifi_timer_setfn,
  ._timer_arm_us = rtems_wifi_timer_arm_us,
  ._wifi_reset_mac = rtems_wifi_reset_mac,
  ._wifi_clock_enable = rtems_wifi_clock_enable,
  ._wifi_clock_disable = rtems_wifi_clock_disable,
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
  ._random = rtems_wifi_random,
  ._slowclk_cal_get = rtems_wifi_slowclk_cal_get,
  ._log_write = rtems_wifi_log_write,
  ._log_writev = rtems_wifi_log_writev,
  ._log_timestamp = rtems_wifi_log_timestamp,
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
  ._coex_init = rtems_wifi_coex_init,
  ._coex_deinit = rtems_wifi_coex_deinit,
  ._coex_enable = rtems_wifi_coex_enable,
  ._coex_disable = rtems_wifi_coex_disable,
  ._coex_status_get = rtems_wifi_coex_status_get,
  ._coex_condition_set = rtems_wifi_stub_coex_condition_set,
  ._coex_wifi_request = rtems_wifi_stub_coex_wifi_request,
  ._coex_wifi_release = rtems_wifi_stub_coex_wifi_release,
  ._coex_wifi_channel_set = rtems_wifi_stub_coex_wifi_channel_set,
  ._coex_event_duration_get = rtems_wifi_stub_coex_event_duration_get,
  ._coex_pti_get = rtems_wifi_coex_pti_get,
  ._coex_schm_status_bit_clear = rtems_wifi_coex_schm_status_bit_clear,
  ._coex_schm_status_bit_set = rtems_wifi_coex_schm_status_bit_set,
  ._coex_schm_interval_set = rtems_wifi_coex_schm_interval_set,
  ._coex_schm_interval_get = rtems_wifi_stub_coex_schm_interval_get,
  ._coex_schm_curr_period_get = rtems_wifi_stub_coex_schm_curr_period_get,
  ._coex_schm_curr_phase_get = rtems_wifi_stub_coex_schm_curr_phase_get,
  ._coex_schm_process_restart = rtems_wifi_stub_coex_schm_process_restart,
  ._coex_schm_register_cb = rtems_wifi_coex_schm_register_cb,
  ._coex_register_start_cb = rtems_wifi_coex_register_start_cb,
  ._coex_schm_flexible_period_set = rtems_wifi_stub_coex_schm_flexible_period_set,
  ._coex_schm_flexible_period_get = rtems_wifi_stub_coex_schm_flexible_period_get,
  ._coex_schm_get_phase_by_idx = rtems_wifi_stub_coex_schm_get_phase_by_idx,
  ._magic = ESP_WIFI_OS_ADAPTER_MAGIC,
};
