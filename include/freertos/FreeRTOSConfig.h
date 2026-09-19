/* Enough of FreeRTOSConfig.h for esp_task.h, which esp_bt.h includes only to
 * derive the controller task's stack size and priority from
 * configMAX_PRIORITIES.  Nothing here runs; the values decide two fields of
 * the controller config, and rtems_esp_bt.c maps the priority to an RTEMS one.
 */
#pragma once
#define configMAX_PRIORITIES 25
