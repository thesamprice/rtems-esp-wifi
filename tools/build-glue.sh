#!/bin/sh
set -eu
SP=/private/tmp/claude-501/-Users-sprice5-src-rtems-builder/9e0245b3-c6f3-4565-81ab-2ab4cf32d4af/scratchpad
LIB=$SP/iram-prefix/riscv-rtems7/esp32c3db/lib
export PATH=$HOME/rtems/7/bin:$PATH
riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" \
  -Wall -Wextra -Werror -c -o $SP/rtems_esp_glue.o \
  $SP/glue/src/rtems_esp_glue.c
riscv-rtems7-nm --defined-only $SP/rtems_esp_glue.o | grep -E " [TDRB] " | awk '{print "  "$3}'
