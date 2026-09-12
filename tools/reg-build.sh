#!/bin/sh
set -eu
SP=/private/tmp/claude-501/-Users-sprice5-src-rtems-builder/9e0245b3-c6f3-4565-81ab-2ab4cf32d4af/scratchpad
LIB=$SP/iram-prefix/riscv-rtems7/esp32c3db/lib
HAL=$SP/hal/components
export PATH=$HOME/rtems/7/bin:$PATH
riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" \
  -I $SP/glue/include/rtems-esp -I $SP/glue/include \
  -I $HAL/esp_wifi/include -I $HAL/esp_wifi/include/local \
  -I $HAL/esp_common/include -I $HAL/esp_event/include \
  -I $HAL/esp_hw_support/include -I $HAL/soc/include \
  -I $HAL/soc/esp32c3/include -I $HAL/esp_rom/include \
  -c -o $SP/reg.o $HAL/esp_wifi/regulatory/esp_wifi_regulatory.c 2>&1 | head -8
