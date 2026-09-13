#!/bin/sh
# Build wpa_supplicant for riscv-rtems7 and archive what compiles.
#
# 118 of the 147 sources build with the CONFIG_* set in
# include/rtems-esp/sdkconfig.h.  The 29 that do not are DPP (Wi-Fi Easy
# Connect), NAN, and the SAE/EAP paths that need mbedtls' elliptic-curve and
# TLS halves -- none of which a WPA2-PSK station uses.
#
# Sources that do not compile are SKIPPED and listed, never silently dropped:
# the link is the arbiter, and anything genuinely needed shows up there as an
# undefined symbol rather than as silence.
set -eu
SP=${SP:?set SP to the work directory}
TOP=${TOP:-/Users/sprice5/src/rtems-esphome}
W=$TOP/src/esp-hal-3rdparty/components/wpa_supplicant
HAL=$TOP/src/esp-hal-3rdparty/components
G=$TOP/src/rtems-esp-wifi
M=$TOP/src/mbedtls
LIB=$SP/wifi-prefix/riscv-rtems7/esp32c3db/lib
OUT=$SP/wpa/all
export PATH=$HOME/rtems/7/bin:$PATH
rm -rf "$OUT"; mkdir -p "$OUT"

# -DESP_PLATFORM gates the u8/u16/u32 typedefs in src/utils/common.h.
# src/utils is needed explicitly: 76 sources include "includes.h" from other
# directories, where the compiler's relative-to-including-file search misses it.
INC="-isystem $LIB/include -I $G/include -I $G/include/rtems-esp
     -I $W/src -I $W/src/utils -I $W/src/common -I $W/include
     -I $W/port/include -I $W/esp_supplicant/include -I $W/esp_supplicant/src
     -I $M/include -I $M/tf-psa-crypto/include
     -I $M/tf-psa-crypto/drivers/builtin/include
     -I $HAL/esp_wifi/include -I $HAL/esp_wifi/wifi_apps/roaming_app/include
     -I $HAL/esp_common/include -I $HAL/log/include -I $HAL/esp_event/include
     -I $HAL/esp_rom/include -I $HAL/esp_rom/esp32c3/include/esp32c3
     -I $HAL/soc/include -I $HAL/soc/esp32c3/include
     -I $HAL/esp_hw_support/include -I $HAL/esp_timer/include
     -I $HAL/esp_system/include"

built=0; skipped=""
for c in $W/src/*/*.c $W/esp_supplicant/src/*.c $W/esp_supplicant/src/*/*.c $W/port/*.c; do
  [ -f "$c" ] || continue
  o="$OUT/$(echo "${c#$W/}" | tr '/' '_' | sed 's/\.c$/.o/')"
  if riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -DESP_PLATFORM \
       -DMBEDTLS_CONFIG_FILE='<rtems-esp/mbedtls_config.h>' \
       $INC -Os -ffunction-sections -fdata-sections \
       -c -o "$o" "$c" 2>"$o.err"; then
    built=$((built+1))
  else
    skipped="$skipped $(basename "$c")"; rm -f "$o"
  fi
done

rm -f "$SP/libwpa.a"
riscv-rtems7-ar rcs "$SP/libwpa.a" "$OUT"/*.o
echo "compiled $built sources into libwpa.a"
[ -z "$skipped" ] || { echo "not built under this configuration:"; for s in $skipped; do echo "    $s"; done; }

# The two symbols the port stopped defining itself must come from here.
for s in esp_supplicant_init g_wifi_default_wpa_crypto_funcs; do
  riscv-rtems7-nm --defined-only "$SP/libwpa.a" 2>/dev/null | grep -qw "$s" \
    || { echo "libwpa.a does not define $s"; exit 1; }
done
echo "esp_supplicant_init and g_wifi_default_wpa_crypto_funcs are defined"
