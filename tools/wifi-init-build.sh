#!/bin/sh
# Compile ESP-IDF's own wifi_init.c, which is step 5: it is what installs the
# adapter table, starts the PHY and reads calibration.
SP=${SP:?set SP to the work directory}
LIB=$SP/iram-prefix/riscv-rtems7/esp32c3db/lib
HAL=$SP/hal/components
export PATH=$HOME/rtems/7/bin:$PATH
riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" \
  -I $SP/glue/include/rtems-esp -I $SP/glue/include \
  -I $HAL/esp_wifi/include -I $HAL/esp_wifi/include/local \
  -I $HAL/esp_common/include -I $HAL/esp_event/include \
  -I $HAL/log/include -I $HAL/esp_phy/include -I $HAL/esp_timer/include \
  -I $HAL/esp_system/include -I $HAL/heap/include -I $HAL/hal/include \
  -I $HAL/esp_coex/include -I $HAL/esp_wifi/wifi_apps/include \
  -I $HAL/esp_hw_support/include -I $HAL/esp_hw_support/include/soc \
  -I $HAL/soc/include -I $HAL/soc/esp32c3/include -I $HAL/esp_rom/include \
  -c -o $SP/wifi_init.o $HAL/esp_wifi/src/wifi_init.c 2>&1
