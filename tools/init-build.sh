#!/bin/sh
# Compile the port's own esp_wifi_init.
SP=${SP:?set SP to the work directory}
LIB=$SP/iram-prefix/riscv-rtems7/esp32c3db/lib
HAL=$SP/hal/components
export PATH=$HOME/rtems/7/bin:$PATH
riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" \
  -I $SP/glue/include/rtems-esp -I $SP/glue/include \
  -I $HAL/esp_wifi/include -I $HAL/esp_wifi/include/local \
  -I $HAL/esp_common/include -I $HAL/esp_event/include \
  -I $HAL/log/include -I $HAL/esp_phy/include -I $HAL/esp_timer/include \
  -I $HAL/esp_hw_support/include -I $HAL/soc/include \
  -I $HAL/soc/esp32c3/include -I $HAL/esp_rom/include \
  -Wall -c -o $SP/wifi_init_rtems.o $SP/glue/src/rtems_esp_wifi_init.c 2>&1
