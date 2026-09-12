#!/bin/sh
SP=/private/tmp/claude-501/-Users-sprice5-src-rtems-builder/9e0245b3-c6f3-4565-81ab-2ab4cf32d4af/scratchpad
LD=$SP/hal/components/esp_rom/esp32c3/ld
export PATH=$HOME/rtems/7/bin:$PATH
riscv-rtems7-ld -o $SP/romlink2.elf $SP/romtest.o $SP/rtems_esp_glue.o \
  --whole-archive $SP/blobs/libnet80211.a $SP/blobs/libpp.a $SP/blobs/libcore.a \
  $SP/blobs/libphy.a $SP/blobs/libmesh.a $SP/blobs/libespnow.a \
  $SP/blobs/libsmartconfig.a $SP/blobs/libwapi.a --no-whole-archive \
  -T $LD/esp32c3.rom.ld -T $LD/esp32c3.rom.api.ld -T $LD/esp32c3.rom.libgcc.ld \
  -T $LD/esp32c3.rom.newlib.ld -T $LD/esp32c3.rom.libc.ld \
  -T $LD/esp32c3.rom.version.ld -T $LD/esp32c3.rom.eco3.ld \
  --unresolved-symbols=report-all 2>&1
