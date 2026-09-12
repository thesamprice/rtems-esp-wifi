#!/bin/sh
set -eu
SP=/private/tmp/claude-501/-Users-sprice5-src-rtems-builder/9e0245b3-c6f3-4565-81ab-2ab4cf32d4af/scratchpad
LIB=$SP/iram-prefix/riscv-rtems7/esp32c3db/lib
HAL=$SP/hal/components
export PATH=$HOME/rtems/7/bin:$PATH
riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" \
  -I $SP/glue/include/rtems-esp \
  -I $HAL/esp_wifi/include \
  -c -o $SP/ftm.o $HAL/esp_wifi/src/ftm_load_calibration.c
riscv-rtems7-nm --defined-only $SP/ftm.o | grep -c " [DBR] "
