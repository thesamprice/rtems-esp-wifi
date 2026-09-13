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
#
# The rest of this list is the component's own PRIV_INCLUDE_DIRS, copied from
# components/wpa_supplicant/CMakeLists.txt rather than grown one error at a
# time.  src/crypto is the one that matters: crypto_mbedtls.c includes
# "crypto.h", which lives there and nowhere else.
INC="-isystem $LIB/include -I $G/include -I $G/include/rtems-esp
     -I $W/src -I $W/src/utils -I $W/src/common -I $W/src/crypto
     -I $W/include -I $W/port/include
     -I $W/esp_supplicant/include -I $W/esp_supplicant/include/esp_private
     -I $W/esp_supplicant/src
     -I $M/include -I $M/tf-psa-crypto/include
     -I $M/tf-psa-crypto/drivers/builtin/include
     -I $HAL/esp_wifi/include -I $HAL/esp_wifi/wifi_apps/roaming_app/include
     -I $HAL/esp_wifi/wifi_apps/roaming_app/src
     -I $HAL/esp_wifi/wifi_apps/include/apps_private
     -I $HAL/esp_common/include -I $HAL/log/include -I $HAL/esp_event/include
     -I $HAL/esp_rom/include -I $HAL/esp_rom/esp32c3/include
     -I $HAL/esp_rom/esp32c3/include/esp32c3
     -I $HAL/soc/include -I $HAL/soc/esp32c3/include
     -I $HAL/esp_hw_support/include -I $HAL/esp_timer/include
     -I $HAL/esp_system/include"

# The supplicant's feature macros.
#
# These are NOT sdkconfig.h symbols and cannot be set there.  Only 11 of the
# supplicant's 400-odd files include sdkconfig.h at all, so a #define there
# reaches some translation units and not others -- which is how ccmp.c came to
# be compiled with CONFIG_IEEE80211W *off* while wpa.h saw it on.  Upstream
# passes them with target_compile_definitions(... PRIVATE ...), so they are
# passed here the same way: once, to every file, from one list.
#
# include/rtems-esp/sdkconfig.h holds the Kconfig-level CONFIG_ESP_WIFI_*
# choices; this list is the translation of those choices, and it mirrors
# components/wpa_supplicant/CMakeLists.txt line for line.  When a symbol here
# has no Kconfig test in front of it upstream, it has no comment here either:
# it is unconditional there.
DEFS="-D__ets__ -DESP_SUPPLICANT -DIEEE8021X_EAPOL -DEAP_PEER_METHOD
      -DEAP_MSCHAPv2 -DEAP_TTLS -DEAP_TLS -DEAP_PEAP -DUSE_WPA2_TASK
      -DCONFIG_WPS -DESPRESSIF_USE -DCONFIG_ECC -DCONFIG_IEEE80211W
      -DCONFIG_SHA256 -DCONFIG_NO_RADIUS"

# CONFIG_ESP_WIFI_MBEDTLS_CRYPTO=n, so no -DCONFIG_CRYPTO_MBEDTLS and none of
# the four esp_supplicant/src/crypto/crypto_mbedtls*.c files.  This is not a
# preference; see include/rtems-esp/sdkconfig.h and docs/step6-supplicant.md
# for why crypto_mbedtls.c cannot be compiled against a bare espressif/mbedtls.
#
# CONFIG_CRYPTO_INTERNAL follows upstream's own rule -- CMakeLists.txt defines
# it when neither CONFIG_MBEDTLS_SHA1_C nor CONFIG_MBEDTLS_HARDWARE_SHA is set,
# and this port has neither.  It is what makes src/crypto/sha1-internal.c
# define sha1_vector(), which crypto_ops.c puts in the crypto table and the
# WiFi libraries call for the EAPOL-Key MIC.
DEFS="$DEFS -DCONFIG_CRYPTO_INTERNAL"

built=0; skipped=""
for c in $W/src/*/*.c $W/esp_supplicant/src/*.c $W/esp_supplicant/src/*/*.c $W/port/*.c; do
  [ -f "$c" ] || continue
  o="$OUT/$(echo "${c#$W/}" | tr '/' '_' | sed 's/\.c$/.o/')"
  # -std=gnu17 is required, not tidiness.  esp_wps.c initialises an atomic_int
  # with ATOMIC_VAR_INIT(), which C17 deprecated and C23 removed; this gcc
  # defaults to gnu23 (__STDC_VERSION__ 202311L) and its stdatomic.h hides the
  # macro behind __STDC_VERSION__ <= 201710L.  Without this the file does not
  # compile and wps_get_wps_sm_cb() -- which esp_wpa_main.c calls
  # unconditionally -- is undefined at link.
  if riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -std=gnu17 -DESP_PLATFORM $DEFS \
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
