/* Supplies what <rtems/confdefs.h> generates, so the probe's undefined list is
 * the symbols nothing in the tree provides rather than the ones an application
 * always does. */
#include <rtems.h>
extern void *g_wifi_osi_funcs_ref;
static rtems_task Init( rtems_task_argument arg ) { (void) arg; }
#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_MAXIMUM_TASKS 16
#define CONFIGURE_MAXIMUM_SEMAPHORES 32
#define CONFIGURE_MAXIMUM_MESSAGE_QUEUES 16
#define CONFIGURE_MAXIMUM_POSIX_KEYS 8
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
