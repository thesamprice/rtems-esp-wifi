#!/bin/sh
# The port's event path: the handler table, the dispatch task, and what the OS
# adapter's _event_post calls.
SP=${SP:?set SP to the work directory}
LIB=$SP/iram-prefix/riscv-rtems7/esp32c3db/lib
HAL=$SP/hal/components
export PATH=$HOME/rtems/7/bin:$PATH
riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" \
  -I $SP/glue/include -I $SP/glue/include/rtems-esp \
  -I $HAL/esp_wifi/include -I $HAL/esp_common/include \
  -I $HAL/esp_event/include -I $HAL/log/include \
  -I $HAL/esp_rom/include -I $HAL/soc/include -I $HAL/soc/esp32c3/include \
  -I $HAL/esp_hw_support/include -I $HAL/esp_timer/include \
  -Wall -c -o $SP/esp_event_rtems.o $SP/glue/src/rtems_esp_event.c 2>&1
