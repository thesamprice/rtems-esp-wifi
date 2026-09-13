#!/bin/sh
# Compile the netif: src/rtems_esp_netif.c, which is the one translation unit
# in this repository that needs both ESP-IDF's headers and lwIP's.
#
# Read tools/event-build.sh first; this is the same shape with two additions,
# and both of them are findings rather than conveniences.
#
# ADDITION 1: lwIP does not come from the BSP prefix, because it is not there.
#
# The prefix this port builds against -- iram-prefix, built from
# configs/config_esp32c3db_iram.ini -- installs librtemsbsp, librtemscpu,
# librtemscxx, librtemsdefaultconfig, librtemstest, libftpd, libftpfs,
# libjffs2, libtftpfs and libz.  There is no liblwip.a and no lwip/ under
# lib/include.  rtems-lwip is a separate package with its own waf build, and it
# has never been built for this BSP: defs/bsps/ in it has aarch64, arm and
# sparc and no riscv at all, so `./waf configure --rtems-bsps=riscv/esp32c3db`
# has nothing to read.
#
# So the headers here come from an rtems-lwip *source* checkout.  They are the
# real port's real headers, in the same four include paths that
# defs/common/lwip.json hands to its own build, so what compiles here compiles
# there.  What this cannot do is link, because there is no liblwip.a to link
# against.  docs/step8-netif.md states plainly which of the two that leaves
# verified.
#
# ADDITION 2: lwipopts.h wants two headers that only a real build produces.
#
# rtemslwip/include/lwipopts.h includes <lwipconfig.h>, which waf generates
# from config.ini and which is an empty guard when no options are set, and
# <lwipbspopts.h>, which is per-BSP and does not exist for esp32c3db.  This
# script writes both into the build directory so the compile can proceed, and
# the lwipbspopts.h it writes is not a placeholder: the numbers in it are the
# ones this part needs, and they are in docs/step8-netif.md as the content of
# the rtems-lwip file that has to exist before any of this runs on hardware.
# rtems-lwip's defaults are MEM_SIZE 2 MiB and PBUF_POOL_SIZE 512 at
# PBUF_POOL_BUFSIZE 1600, which is 2.8 MiB of static and heap demand on a part
# with 400 KiB of SRAM.
set -e

SP=${SP:?set SP to the work directory}
GLUE=${GLUE:-$SP/glue}
HAL=${HAL:-$SP/hal}/components
LWIP=${LWIP:-$SP/rtems-lwip}
LIB=${LIB:-$SP/iram-prefix/riscv-rtems7/esp32c3db/lib}
OUT=${OUT:-$SP/netif-build}

export PATH=$HOME/rtems/7/bin:$PATH

# The existence check earns its place here for the reason tools/applink.sh
# records: three times a missing input made the build fail on its first
# argument and report nothing, which reads as success.  Everything checked is
# something used below, so the list cannot drift from the command.
missing=
for f in \
  "$GLUE/src/rtems_esp_netif.c" \
  "$GLUE/include/rtems-esp/netif.h" \
  "$LIB/include/rtems.h" \
  "$HAL/esp_wifi/include/esp_wifi.h" \
  "$HAL/esp_wifi/include/esp_private/wifi.h" \
  "$LWIP/lwip/src/include/lwip/netif.h" \
  "$LWIP/rtemslwip/include/lwipopts.h" \
  "$LWIP/rtemslwip/include/arch/cc.h"
do
  [ -r "$f" ] || missing="$missing $f"
done
if [ -n "$missing" ]; then
  echo "netif-build.sh: cannot build, missing:" >&2
  for f in $missing; do echo "  $f" >&2; done
  echo "(SP=$SP GLUE=$GLUE HAL=$HAL LWIP=$LWIP)" >&2
  echo "LWIP must point at an rtems-lwip checkout with its lwip submodule" >&2
  echo "populated; there is no lwIP in the BSP prefix, see the header above." >&2
  exit 2
fi

mkdir -p "$OUT/lwipopts"

# Empty, exactly as waf's write_config_header produces it when config.ini sets
# no lwIP options for the BSP -- compare
# rtems-lwip/build/arm-rtems7-xilinx_zynq_a9_qemu/rtemslwip/include/lwipconfig.h.
cat > "$OUT/lwipopts/lwipconfig.h" <<'EOF'
/* WARNING! All changes made to this file will be lost! */

#ifndef CONFIGURED_LWIP_BSP_OPTS_H
#define CONFIGURED_LWIP_BSP_OPTS_H

#endif /* CONFIGURED_LWIP_BSP_OPTS_H */
EOF

# The ESP32-C3's lwIP options.  See docs/step8-netif.md; this belongs in
# rtems-lwip as rtemslwip/esp32c3/lwipbspopts.h and is written here only
# because that file does not exist yet.
cat > "$OUT/lwipopts/lwipbspopts.h" <<'EOF'
#ifndef RTEMSLWIP_LWIPBSPOPTS_H
#define RTEMSLWIP_LWIPBSPOPTS_H

/*
 * The part has 400 KiB of SRAM, of which the WiFi libraries already claim
 * 54 KiB and their RX/TX buffer counts a good deal more.  rtems-lwip's own
 * defaults -- MEM_SIZE 2 MiB, PBUF_POOL_SIZE 512 at 1600 bytes -- cannot fit
 * and are not close to fitting.
 */
#define LWIP_ETHERNET 1
#define LWIP_TCPIP_CORE_LOCKING 1

#define MEM_ALIGNMENT __SIZEOF_POINTER__
#define MEM_LIBC_MALLOC 1
#define MEMP_MEM_MALLOC 1
#define MEM_SIZE ( 32 * 1024 )

/*
 * 1600 is rtems-lwip's own bufsize and covers a 1518-byte frame in one pbuf,
 * which keeps the receive path's pbuf_take() a single memcpy.  Sixteen of them
 * is 25 KiB.
 */
#define PBUF_POOL_SIZE 16
#define PBUF_POOL_BUFSIZE 1600

#endif /* RTEMSLWIP_LWIPBSPOPTS_H */
EOF

# The four lwIP include paths are defs/common/lwip.json's
# header-paths-to-import, in its order, plus the generated pair first so they
# are found before anything else offers a same-named header.
riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 \
  -isystem "$LIB/include" \
  -I "$OUT/lwipopts" \
  -I "$LWIP/rtemslwip/bsd_compat_include" \
  -I "$LWIP/lwip/src/include" \
  -I "$LWIP/rtemslwip/include/arch" \
  -I "$LWIP/rtemslwip/include" \
  -I "$GLUE/include" -I "$GLUE/include/rtems-esp" \
  -I "$HAL/esp_wifi/include" -I "$HAL/esp_wifi/include/local" \
  -I "$HAL/esp_common/include" -I "$HAL/esp_event/include" \
  -I "$HAL/log/include" -I "$HAL/esp_phy/include" \
  -I "$HAL/esp_timer/include" -I "$HAL/esp_system/include" \
  -I "$HAL/heap/include" -I "$HAL/hal/include" \
  -I "$HAL/esp_coex/include" -I "$HAL/esp_wifi/wifi_apps/include" \
  -I "$HAL/esp_hw_support/include" -I "$HAL/esp_hw_support/include/soc" \
  -I "$HAL/soc/include" -I "$HAL/soc/esp32c3/include" \
  -I "$HAL/esp_rom/include" \
  -Wall -Wextra -Werror \
  -c -o "$OUT/rtems_esp_netif.o" "$GLUE/src/rtems_esp_netif.c"

echo "netif-build.sh: built $OUT/rtems_esp_netif.o"

# The three blob entry points, resolved out of the archives rather than taken
# on trust.  A netif that compiles against the wrong signature still compiles;
# what says the seam is real is that these symbols exist where wifi.h says.
if [ -d "$SP/blobs" ]; then
  echo "netif-build.sh: the blob side of the seam:"
  for s in esp_wifi_internal_tx esp_wifi_internal_reg_rxcb \
           esp_wifi_internal_free_rx_buffer esp_wifi_get_mac
  do
    found=$( riscv-rtems7-nm -A --defined-only "$SP"/blobs/*.a 2>/dev/null |
             grep -w "$s" | head -1 )
    echo "  ${found:-$s NOT FOUND}"
  done

  # And that the object references exactly those and nothing else out of the
  # WiFi libraries.  A netif reaching further into the blobs than the four
  # names above has grown a dependency the substitution seam cannot cover, and
  # this line is what would notice.
  echo "netif-build.sh: what the object asks of esp_wifi:"
  riscv-rtems7-nm -u "$OUT/rtems_esp_netif.o" | grep esp_wifi || echo "  (none)"
fi
