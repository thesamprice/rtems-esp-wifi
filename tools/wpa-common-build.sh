#!/bin/sh
# wpa_supplicant's src/utils/common.c, which owns hexstr2bin -- the one symbol
# the blobs want from the supplicant that is not behind esp_supplicant_init().
SP=${SP:?set SP to the work directory}
LIB=$SP/iram-prefix/riscv-rtems7/esp32c3db/lib
HAL=$SP/hal/components
export PATH=$HOME/rtems/7/bin:$PATH
# -DESP_PLATFORM is what gates the u8/u16/u32 typedefs in src/utils/common.h,
# and ESP-IDF defines it for every component rather than per-file.
riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -DESP_PLATFORM -isystem "$LIB/include" \
  -I $SP/glue/include/rtems-esp -I $SP/glue/include \
  -I $HAL/wpa_supplicant/src -I $HAL/wpa_supplicant/src/utils \
  -I $HAL/wpa_supplicant/include -I $HAL/wpa_supplicant/port/include \
  -I $HAL/esp_wifi/include -I $HAL/esp_common/include -I $HAL/log/include \
  -I $HAL/esp_event/include -I $HAL/esp_rom/include \
  -I $HAL/soc/include -I $HAL/soc/esp32c3/include \
  -I $HAL/esp_hw_support/include -I $HAL/esp_timer/include \
  -c -o $SP/wpa_common.o $HAL/wpa_supplicant/src/utils/common.c 2>&1
