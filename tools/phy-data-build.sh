#!/bin/sh
# The ESP32-C3's PHY init data, which esp-hal-3rdparty ships as source.  It is
# the per-chip PHY register set; there is no way to derive it and no default,
# so register_chipv7_phy() needs this object.
SP=${SP:?set SP to the work directory}
LIB=$SP/iram-prefix/riscv-rtems7/esp32c3db/lib
HAL=$SP/hal/components
export PATH=$HOME/rtems/7/bin:$PATH
riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" \
  -I $SP/glue/include/rtems-esp -I $SP/glue/include \
  -I $HAL/esp_phy/esp32c3/include -I $HAL/esp_phy/include \
  -I $HAL/esp_wifi/include -I $HAL/esp_common/include \
  -I $HAL/esp_hw_support/include -I $HAL/soc/include \
  -I $HAL/soc/esp32c3/include -I $HAL/esp_rom/include -I $HAL/log/include \
  -c -o $SP/phy_init_data.o $HAL/esp_phy/esp32c3/phy_init_data.c 2>&1
