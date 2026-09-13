#!/bin/sh
# Build mbedtls for riscv-rtems7 with the port's configuration.
#
# Espressif's mbedtls is the 4.x layout: the classic library/ plus a
# tf-psa-crypto/ tree holding the PSA core and the builtin drivers.  Both are
# needed; include/rtems-esp/mbedtls_config.h says which algorithms.
#
# Everything goes into one archive so the linker pulls only the objects that
# are actually referenced -- the config decides what is *available*, the link
# decides what is *paid for*.
set -eu
SP=${SP:?set SP to the work directory}
TOP=${TOP:-/Users/sprice5/src/rtems-esphome}
M=$TOP/src/mbedtls
G=$TOP/src/rtems-esp-wifi
LIB=$SP/wifi-prefix/riscv-rtems7/esp32c3db/lib
OUT=$SP/mbedtls-obj
export PATH=$HOME/rtems/7/bin:$PATH
mkdir -p "$OUT"

INC="-isystem $LIB/include -I $G/include
     -I $M/include -I $M/library
     -I $M/tf-psa-crypto/include -I $M/tf-psa-crypto/drivers/builtin/include
     -I $M/tf-psa-crypto/core -I $M/tf-psa-crypto/drivers/builtin/src
     -I $M/tf-psa-crypto/utilities
     -I $M/3rdparty/everest/include -I $M/3rdparty/p256-m"

# Sources that do not compile under this configuration are SKIPPED, not fatal.
#
# This is mbedtls 4.x, where the legacy bignum/ECP/RSA implementations moved to
# drivers/builtin/include/mbedtls/private/ and are selected by the CMake build
# per configuration.  A PSA-based config like this one routes ECC through the
# PSA layer instead, so ecp.c, bignum.c and rsa.c reference headers that are
# deliberately not on the include path -- they are not part of this build.
#
# Skipping is only safe because the LINK is the arbiter: if something skipped
# here is actually needed, it shows up as an undefined symbol rather than as
# silence.  tools/applink.sh is that check.  The skipped list is printed every
# time so it cannot quietly grow.
#
# tf-psa-crypto/platform is one file, platform_util.c, and it is the home of
# mbedtls_platform_zeroize().  27 of the objects built here reference it, so
# leaving the directory out produces an archive that cannot satisfy its own
# members -- and says nothing about it until something pulls one of the 27 in.
skipped=""
for c in $M/library/*.c $M/tf-psa-crypto/core/*.c \
         $M/tf-psa-crypto/utilities/*.c \
         $M/tf-psa-crypto/platform/*.c \
         $M/tf-psa-crypto/drivers/builtin/src/*.c; do
  o="$OUT/$(basename "$(dirname "$c")")_$(basename "$c" .c).o"
  if ! riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 \
        -DMBEDTLS_CONFIG_FILE='<rtems-esp/mbedtls_config.h>' \
        $INC -Os -ffunction-sections -fdata-sections \
        -c -o "$o" "$c" 2>"$OUT/$(basename "$c").err"; then
    skipped="$skipped $(basename "$c")"
    rm -f "$o"
  fi
done

if [ -n "$skipped" ]; then
  echo "not built under this configuration:"
  for s in $skipped; do echo "    $s"; done
fi

rm -f "$SP/libmbedtls.a"
riscv-rtems7-ar rcs "$SP/libmbedtls.a" "$OUT"/*.o
riscv-rtems7-size --totals "$SP/libmbedtls.a" 2>/dev/null | tail -1
echo "libmbedtls.a: $(ls -l "$SP/libmbedtls.a" | awk '{print $5}') bytes, $(ls "$OUT"/*.o | wc -l) objects"
