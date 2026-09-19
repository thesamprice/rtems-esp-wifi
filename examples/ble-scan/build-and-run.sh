#!/bin/sh
set -eu
SP=/private/tmp/claude-501/-Users-sprice5-src-rtems-builder/9e0245b3-c6f3-4565-81ab-2ab4cf32d4af/scratchpad/ble
H=/Users/sprice5/src/rtems-esphome/src/esp-hal-3rdparty/components
G=/Users/sprice5/src/rtems-esphome/src/rtems-esp-wifi
P=/Users/sprice5/src/rtems-esphome/src/esp-phy-lib/esp32c3
LIB=$SP/prefix/riscv-rtems7/esp32c3db/lib
export PATH=$HOME/rtems/7/bin:$PATH

INC="-I $SP -I $SP/shim -I $G/include -I $G/include/rtems-esp \
 -I $H/bt/include/esp32c3/include -I $H/bt/include/common \
 -I $H/esp_common/include -I $H/log/include -I $H/esp_rom/include \
 -I $H/soc/include -I $H/soc/esp32c3/include -I $H/esp_hw_support/include \
 -I $H/hal/include -I $H/esp_system/include -I $H/esp_system/port/include \
 -I $H/riscv/include -I $H/esp_phy/include"

CC="riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -O2 -Wall"

case "${1:-all}" in
probe)
  $CC -c $INC -isystem $LIB/include -o $SP/probe.o $SP/probe.c
  riscv-rtems7-nm $SP/probe.o | head
  ;;
glue)
  $CC -c $INC -isystem $LIB/include -o $SP/rtems_esp_bt.o $G/src/rtems_esp_bt.c
  echo "glue ok"
  ;;
app)
  # linkcmds with the blob section placement spliced in
  mkdir -p "$SP/ldir"
  awk -v base="$LIB/linkcmds.base" -v frag="$G/ld/esp32c3-wifi-sections.ld" '
    /^[[:space:]]*INCLUDE[[:space:]]+linkcmds\.base/ {
      while ((getline line < base) > 0) {
        if (line ~ /^[[:space:]]*\.bss[[:space:]]*:/ && !spliced) {
          while ((getline f < frag) > 0) print f
          spliced = 1
        }
        print line
      }
      next
    }
    { print }
  ' "$LIB/linkcmds" > "$SP/ldir/linkcmds"
  grep -q 'wifi_iram' "$SP/ldir/linkcmds" || { echo "placement did not splice" >&2; exit 2; }

  $CC -c $INC -isystem $LIB/include -o $SP/ble_scan.o $SP/ble_scan.c
  $CC -c $INC -isystem $LIB/include -Dcoex_pti_print=rtems_esp_unused_coex_pti_print -o $SP/rtems_esp_glue.o $G/src/rtems_esp_glue.c
  $CC -isystem $LIB/include -B "$SP/ldir" -B "$LIB" -qrtems \
    -o $SP/ble-scan.exe \
    $SP/ble_scan.o $SP/rtems_esp_bt.o $SP/rtems_esp_glue.o \
    /private/tmp/claude-501/-Users-sprice5-src-rtems-builder/9e0245b3-c6f3-4565-81ab-2ab4cf32d4af/scratchpad/phy_init_data.o \
    -Wl,--start-group \
    $SP/esp32c3-bt-lib/esp32c3/libbtdm_app.a \
    $P/libbtbb.a $P/libphy.a \
    -Wl,--end-group -lm \
    -T $H/esp_rom/esp32c3/ld/esp32c3.rom.ld \
    -T $H/esp_rom/esp32c3/ld/esp32c3.rom.bt_funcs.ld \
    -T $H/esp_rom/esp32c3/ld/esp32c3.rom.api.ld \
    -T $H/esp_rom/esp32c3/ld/esp32c3.rom.ble_50.ld \
    -T $H/esp_rom/esp32c3/ld/esp32c3.rom.ble_cca.ld \
    -T $H/esp_rom/esp32c3/ld/esp32c3.rom.ble_dtm.ld \
    -T $H/esp_rom/esp32c3/ld/esp32c3.rom.ble_master.ld \
    -T $H/esp_rom/esp32c3/ld/esp32c3.rom.ble_scan.ld \
    -T $H/esp_rom/esp32c3/ld/esp32c3.rom.ble_smp.ld \
    -T $H/esp_rom/esp32c3/ld/esp32c3.rom.ble_test.ld \
    -T $H/esp_rom/esp32c3/ld/esp32c3.rom.eco3.ld \
    -T $H/esp_rom/esp32c3/ld/esp32c3.rom.eco3_bt_funcs.ld
  riscv-rtems7-objcopy -O binary $SP/ble-scan.exe $SP/ble-scan.raw
  riscv-rtems7-size $SP/ble-scan.exe
  ;;
esac
