#!/bin/sh
# A real application link: gcc, the BSP's start.o and linkcmds, the ROM scripts
# and the blobs.  The earlier probes used bare ld, so linker-script symbols and
# anything <rtems/confdefs.h> generates reported as undefined and buried the
# symbols that are genuinely absent.  What this reports is the actual remaining
# surface.
SP=${SP:-$(cd "$(dirname "$0")/../.." && pwd)}
LIB=$SP/iram-prefix/riscv-rtems7/esp32c3db/lib
LD=$SP/hal/components/esp_rom/esp32c3/ld
export PATH=$HOME/rtems/7/bin:$PATH
# Check the inputs exist before linking.  Without this a wrong SP, or a
# missing fetch, makes ld fail on its first argument -- and the script then
# reports *zero* unresolved symbols, which reads as success.  That has
# happened twice.
set -eu
for f in "$LIB/librtemsbsp.a" "$LD/esp32c3.rom.ld" "$SP/blobs/libpp.a" \
         "$SP/adapter.o" "$SP/confdefs-probe.o"; do
  [ -r "$f" ] || { echo "applink.sh: missing $f (is SP right?)" >&2; exit 2; }
done
set +e

riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -B $LIB -qrtems \
  -o $SP/applink.elf \
  $SP/confdefs-probe.o $SP/rtems_esp_glue.o $SP/ftm.o $SP/reg.o $SP/adapter.o $SP/romtest.o \
  -Wl,--whole-archive \
    $SP/blobs/libnet80211.a $SP/blobs/libpp.a $SP/blobs/libcore.a \
    $SP/blobs/libphy.a $SP/blobs/libmesh.a $SP/blobs/libespnow.a \
    $SP/blobs/libsmartconfig.a $SP/blobs/libwapi.a \
  -Wl,--no-whole-archive \
  -Wl,-T,$LD/esp32c3.rom.ld -Wl,-T,$LD/esp32c3.rom.api.ld \
  -Wl,-T,$LD/esp32c3.rom.libgcc.ld -Wl,-T,$LD/esp32c3.rom.newlib.ld \
  -Wl,-T,$LD/esp32c3.rom.libc.ld -Wl,-T,$LD/esp32c3.rom.version.ld \
  -Wl,-T,$LD/esp32c3.rom.eco3.ld \
  -lm 2>&1

