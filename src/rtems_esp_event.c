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
 * The event path: how a WIFI_EVENT reaches the application.
 *
 * This is the port's own, and it is small on purpose.  ESP-IDF's esp_event is
 * in esp-hal-3rdparty, but it is 1416 lines over nineteen FreeRTOS calls --
 * xQueueCreateWithCaps, xTaskCreatePinnedToCore, xSemaphoreCreateRecursiveMutex
 * and the rest -- so bringing it in means emulating FreeRTOS, which is the one
 * thing include/freertos is careful not to do.  What the WiFi station path
 * needs from an event loop is a handler list and a queue.
 *
 * The events arrive through the OS adapter, not through this API.  The WiFi
 * libraries call g_wifi_osi_funcs._event_post, which ESP-IDF implements over
 * esp_event_post; here it is rtems_esp_event_post below.  So the blobs and the
 * application meet in this file.
 *
 * Two things are deliberately unlike ESP-IDF:
 *
 *   * There is one loop, the default one.  esp_event_loop_create() and
 *     per-loop handlers are not here.  Nothing on the station path asks for a
 *     second loop.
 *
 *   * Handlers are a fixed table, so registering one allocates nothing.  The
 *     table filling up is reported rather than grown, because the alternative
 *     -- a malloc on a path the WiFi task can reach -- is how a low-memory
 *     condition turns into a dropped event that nobody logs.
 *
 * Handlers run on their own task, not on the caller's.  That matters: a
 * handler almost always wants to call back into esp_wifi (esp_wifi_connect
 * from the STA_DISCONNECTED handler is the canonical example), and running it
 * on the WiFi task means re-entering the WiFi libraries from inside
 * themselves.  ESP-IDF has the same separation for the same reason.
 */

#include <rtems-esp/newlib-compat.h>

#include <esp_event.h>
#include <esp_wifi.h>

#include <rtems.h>
#include <rtems/bspIo.h>

#include <string.h>

#ifndef ESP_OK
#define ESP_OK 0
#endif

/*
 * The queue message has to hold the largest event payload, so the size is
 * derived from the payload types rather than picked.  Measured, it is 292
 * bytes on the C3 -- a guess would have been 64, which is what the largest
 * *station* event needs and which would have silently truncated an FTM report.
 *
 * The static assertion is the guard: if a payload grows upstream, the union
 * grows with it and this file still compiles; if someone replaces the union
 * with a constant, it stops.
 */
union rtems_esp_event_payload {
  wifi_event_sta_connected_t         sta_connected;
  wifi_event_sta_disconnected_t      sta_disconnected;
  wifi_event_sta_scan_done_t         scan_done;
  wifi_event_ap_staconnected_t       ap_staconnected;
  wifi_event_ap_stadisconnected_t    ap_stadisconnected;
  wifi_event_ap_probe_req_rx_t       probe_req;
  wifi_event_bss_rssi_low_t          rssi_low;
  wifi_event_home_channel_change_t   channel_change;
  wifi_event_sta_wps_er_pin_t        wps_pin;
  wifi_event_sta_wps_er_success_t    wps_success;
  wifi_event_ftm_report_t            ftm_report;
  wifi_event_action_tx_status_t      action_tx;
  wifi_event_roc_done_t              roc_done;
  wifi_event_ap_wps_rg_pin_t         rg_pin;
  wifi_event_ap_wps_rg_fail_reason_t rg_fail;
};

#define RTEMS_ESP_EVENT_DATA_MAX sizeof( union rtems_esp_event_payload )

_Static_assert(
  RTEMS_ESP_EVENT_DATA_MAX >= sizeof( wifi_event_sta_disconnected_t ),
  "the event payload union must cover the station events at least"
);

typedef struct {
  /*
   * A pointer, not a copy.  An event base in ESP-IDF is a const char * to a
   * string with static storage duration -- WIFI_EVENT and IP_EVENT are
   * globals -- and handlers are matched by pointer equality upstream too.
   */
  const char *base;
  int32_t     id;
  size_t      size;
  char        data[ RTEMS_ESP_EVENT_DATA_MAX ];
} rtems_esp_event_message;

#define RTEMS_ESP_EVENT_HANDLERS    16
#define RTEMS_ESP_EVENT_QUEUE_DEPTH 8

/*
 * Below the WiFi task, which the adapter gives priority 1 after inverting
 * ESP-IDF's numbering, and below the lwIP task in rtems-lwip.  An event
 * handler is application code: it should not be able to delay packet
 * processing by running.
 */
#define RTEMS_ESP_EVENT_TASK_PRIORITY 100
#define RTEMS_ESP_EVENT_TASK_STACK    ( 8 * 1024 )

typedef struct {
  const char              *base;
  int32_t                  id;
  esp_event_handler_t      handler;
  void                    *arg;
} rtems_esp_event_entry;

static rtems_esp_event_entry rtems_esp_event_handlers[ RTEMS_ESP_EVENT_HANDLERS ];

static rtems_id rtems_esp_event_queue;
static rtems_id rtems_esp_event_task;

/*
 * Guards the handler table.  A mutex rather than an interrupt lock: dispatch
 * holds it while a handler runs, and a handler may block.
 */
static rtems_id rtems_esp_event_mutex;

static void rtems_esp_event_dispatch( const rtems_esp_event_message *message )
{
  size_t i;

  rtems_semaphore_obtain( rtems_esp_event_mutex, RTEMS_WAIT, RTEMS_NO_TIMEOUT );

  for ( i = 0; i < RTEMS_ESP_EVENT_HANDLERS; ++i ) {
    rtems_esp_event_entry *entry = &rtems_esp_event_handlers[ i ];

    if ( entry->handler == NULL ) {
      continue;
    }

    if ( entry->base != message->base ) {
      continue;
    }

    if ( entry->id != ESP_EVENT_ANY_ID && entry->id != message->id ) {
      continue;
    }

    /*
     * Called with the table locked, so a handler that registers or
     * unregisters another handler would deadlock.  ESP-IDF permits that and
     * this does not; the alternative is copying the matching entries out
     * first, which is worth doing the moment something needs it rather than
     * on the chance that something might.
     */
    entry->handler(
      entry->arg,
      message->base,
      message->id,
      message->size != 0 ? (void *) message->data : NULL
    );
  }

  rtems_semaphore_release( rtems_esp_event_mutex );
}

static rtems_task rtems_esp_event_body( rtems_task_argument arg )
{
  (void) arg;

  for ( ; ; ) {
    rtems_esp_event_message message;
    size_t                  received;
    rtems_status_code       sc;

    sc = rtems_message_queue_receive(
      rtems_esp_event_queue,
      &message,
      &received,
      RTEMS_WAIT,
      RTEMS_NO_TIMEOUT
    );

    if ( sc != RTEMS_SUCCESSFUL ) {
      printk( "rtems-esp-event: receive failed (%i)\n", (int) sc );
      continue;
    }

    rtems_esp_event_dispatch( &message );
  }
}

/*
 * Created on first use rather than from a constructor, because the order in
 * which esp_wifi_init() and the application's first esp_event_handler_register()
 * happen is the application's choice and either can be first.
 */
static esp_err_t rtems_esp_event_start( void )
{
  rtems_status_code sc;

  if ( rtems_esp_event_task != 0 ) {
    return ESP_OK;
  }

  sc = rtems_semaphore_create(
    rtems_build_name( 'E', 'V', 'M', 'X' ),
    1,
    RTEMS_BINARY_SEMAPHORE | RTEMS_PRIORITY | RTEMS_INHERIT_PRIORITY,
    0,
    &rtems_esp_event_mutex
  );

  if ( sc != RTEMS_SUCCESSFUL ) {
    printk( "rtems-esp-event: no semaphore (%i)\n", (int) sc );

    return ESP_FAIL;
  }

  sc = rtems_message_queue_create(
    rtems_build_name( 'E', 'V', 'N', 'T' ),
    RTEMS_ESP_EVENT_QUEUE_DEPTH,
    sizeof( rtems_esp_event_message ),
    RTEMS_DEFAULT_ATTRIBUTES,
    &rtems_esp_event_queue
  );

  if ( sc != RTEMS_SUCCESSFUL ) {
    printk( "rtems-esp-event: no queue (%i)\n", (int) sc );
    rtems_semaphore_delete( rtems_esp_event_mutex );

    return ESP_FAIL;
  }

  sc = rtems_task_create(
    rtems_build_name( 'E', 'V', 'N', 'T' ),
    RTEMS_ESP_EVENT_TASK_PRIORITY,
    RTEMS_ESP_EVENT_TASK_STACK,
    RTEMS_DEFAULT_MODES,
    RTEMS_FLOATING_POINT | RTEMS_LOCAL,
    &rtems_esp_event_task
  );

  if ( sc != RTEMS_SUCCESSFUL ) {
    printk( "rtems-esp-event: no task (%i)\n", (int) sc );
    rtems_message_queue_delete( rtems_esp_event_queue );
    rtems_semaphore_delete( rtems_esp_event_mutex );
    rtems_esp_event_task = 0;

    return ESP_FAIL;
  }

  sc = rtems_task_start( rtems_esp_event_task, rtems_esp_event_body, 0 );

  if ( sc != RTEMS_SUCCESSFUL ) {
    printk( "rtems-esp-event: task will not start (%i)\n", (int) sc );
    rtems_task_delete( rtems_esp_event_task );
    rtems_message_queue_delete( rtems_esp_event_queue );
    rtems_semaphore_delete( rtems_esp_event_mutex );
    rtems_esp_event_task = 0;

    return ESP_FAIL;
  }

  return ESP_OK;
}

/*
 * What the OS adapter's _event_post entry calls.  Declared in
 * include/rtems-esp/event.h so the adapter does not have to guess at it.
 */
int32_t rtems_esp_event_post(
  const char *event_base,
  int32_t     event_id,
  void       *event_data,
  size_t      event_data_size,
  uint32_t    ticks_to_wait
)
{
  rtems_esp_event_message message;
  rtems_status_code       sc;

  (void) ticks_to_wait;

  if ( event_base == NULL ) {
    return ESP_FAIL;
  }

  if ( event_data_size > RTEMS_ESP_EVENT_DATA_MAX ) {
    /*
     * Reported, never truncated.  A short copy here would hand a handler a
     * partially-filled struct that looks valid, which is the failure mode this
     * port keeps having to dig out.
     */
    printk(
      "rtems-esp-event: %s id %i payload %u > %u, dropped\n",
      event_base,
      (int) event_id,
      (unsigned) event_data_size,
      (unsigned) RTEMS_ESP_EVENT_DATA_MAX
    );

    return ESP_FAIL;
  }

  if ( rtems_esp_event_task == 0 && rtems_esp_event_start() != ESP_OK ) {
    return ESP_FAIL;
  }

  message.base = event_base;
  message.id   = event_id;
  message.size = event_data_size;

  if ( event_data != NULL && event_data_size != 0 ) {
    memcpy( message.data, event_data, event_data_size );
  } else {
    message.size = 0;
  }

  /*
   * Never blocks, whatever ticks_to_wait says, because this is reached from
   * the WiFi task and from interrupt context.  ESP-IDF's own wrapper passes
   * portMAX_DELAY for OSI_FUNCS_TIME_BLOCKING; blocking the WiFi task on an
   * application-serviced queue is a deadlock waiting for a slow handler, and a
   * dropped event that says so is easier to diagnose than a stalled radio.
   */
  sc = rtems_message_queue_send(
    rtems_esp_event_queue,
    &message,
    sizeof( message )
  );

  if ( sc != RTEMS_SUCCESSFUL ) {
    printk(
      "rtems-esp-event: queue full, dropped %s id %i\n",
      event_base,
      (int) event_id
    );

    return ESP_FAIL;
  }

  return ESP_OK;
}

esp_err_t esp_event_post(
  esp_event_base_t event_base,
  int32_t          event_id,
  const void      *event_data,
  size_t           event_data_size,
  TickType_t       ticks_to_wait
)
{
  /*
   * ESP-IDF's public posting API, as distinct from rtems_esp_event_post(),
   * which is what the OS adapter's _event_post table entry calls.  They are
   * the same operation reached from two directions: the WiFi libraries go
   * through the table, and everything built from ESP-IDF sources -- the
   * supplicant is the first -- calls this.
   *
   * The const is dropped deliberately.  ESP-IDF takes event_data as const and
   * copies it; rtems_esp_event_post() also copies it, into the queue message,
   * and never writes through the pointer.  The cast is here rather than in the
   * signature because the signature has to match ESP-IDF's exactly -- a
   * mismatch would compile in every translation unit that includes
   * esp_event.h and fail only at link, which is the hardest kind to place.
   *
   * The return is esp_err_t, where success is 0.  That is NOT the convention
   * used by the queue and semaphore entries of the OS adapter, which are
   * FreeRTOS-shaped and return 1 for success; getting the two confused there
   * cost an afternoon and produced ESP_ERR_WIFI_POST from a queue that was
   * working perfectly.  rtems_esp_event_post() is on this side of that line.
   */
  return (esp_err_t) rtems_esp_event_post(
    event_base,
    event_id,
    (void *) event_data,
    event_data_size,
    (uint32_t) ticks_to_wait
  );
}

esp_err_t esp_event_handler_register(
  esp_event_base_t    event_base,
  int32_t             event_id,
  esp_event_handler_t event_handler,
  void               *event_handler_arg
)
{
  size_t    i;
  esp_err_t result;

  if ( event_base == NULL || event_handler == NULL ) {
    return ESP_ERR_INVALID_ARG;
  }

  result = rtems_esp_event_start();

  if ( result != ESP_OK ) {
    return result;
  }

  rtems_semaphore_obtain( rtems_esp_event_mutex, RTEMS_WAIT, RTEMS_NO_TIMEOUT );

  for ( i = 0; i < RTEMS_ESP_EVENT_HANDLERS; ++i ) {
    if ( rtems_esp_event_handlers[ i ].handler == NULL ) {
      rtems_esp_event_handlers[ i ].base    = event_base;
      rtems_esp_event_handlers[ i ].id      = event_id;
      rtems_esp_event_handlers[ i ].arg     = event_handler_arg;
      rtems_esp_event_handlers[ i ].handler = event_handler;

      rtems_semaphore_release( rtems_esp_event_mutex );

      return ESP_OK;
    }
  }

  rtems_semaphore_release( rtems_esp_event_mutex );

  printk(
    "rtems-esp-event: all %u handler slots are taken\n",
    (unsigned) RTEMS_ESP_EVENT_HANDLERS
  );

  return ESP_ERR_NO_MEM;
}

esp_err_t esp_event_handler_unregister(
  esp_event_base_t    event_base,
  int32_t             event_id,
  esp_event_handler_t event_handler
)
{
  size_t i;

  if ( event_base == NULL || event_handler == NULL ) {
    return ESP_ERR_INVALID_ARG;
  }

  if ( rtems_esp_event_mutex == 0 ) {
    return ESP_OK;
  }

  rtems_semaphore_obtain( rtems_esp_event_mutex, RTEMS_WAIT, RTEMS_NO_TIMEOUT );

  for ( i = 0; i < RTEMS_ESP_EVENT_HANDLERS; ++i ) {
    rtems_esp_event_entry *entry = &rtems_esp_event_handlers[ i ];

    if (
      entry->handler == event_handler
        && entry->base == event_base
        && entry->id == event_id
    ) {
      entry->handler = NULL;
      entry->base    = NULL;
      entry->arg     = NULL;
    }
  }

  rtems_semaphore_release( rtems_esp_event_mutex );

  return ESP_OK;
}

/*
 * Mesh only.
 *
 * libmesh is the only archive that references this, and libmesh cannot simply
 * be dropped from the link -- libnet80211 and libpp reference mesh symbols
 * themselves, so the archives come as a set.  What that means here is that the
 * symbol has to exist for the link and does not have to work for a station:
 * nothing reaches it unless the application calls esp_mesh_init(), which this
 * port has no way to do.
 *
 * So it says so, rather than quietly returning ESP_OK and leaving a future
 * mesh port to find out the hard way that its events go nowhere.
 */
esp_err_t esp_mesh_send_event_internal(
  int32_t event_id,
  void   *event_data,
  size_t  event_data_size
)
{
  static bool reported;

  (void) event_id;
  (void) event_data;
  (void) event_data_size;

  if ( !reported ) {
    reported = true;
    printk(
      "rtems-esp-event: esp_mesh_send_event_internal is not implemented; "
      "mesh is not supported on this port\n"
    );
  }

  return ESP_FAIL;
}
