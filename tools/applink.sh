#!/bin/sh
# A real application link: gcc, the BSP's start.o and linkcmds, the ROM
# scripts, the blobs, libm and what <rtems/confdefs.h> generates.  The earlier
# probes used bare ld with none of that, so every rtems_* and libc symbol the
# port itself calls reported as undefined and buried the symbols that are
# genuinely absent -- adding a *working* file took the count from 22 to 50.
#
# OBJS and LIBS are variables so the existence check below covers exactly what
# the link uses.  That check earns its place: three times, a missing input made
# ld fail on its first argument, and the script then reported *zero* unresolved
# symbols -- which reads as success.  Twice the input was missing because SP was
# wrong; the third time the check itself was a hand-written list that had not
# been updated when an object was added, so listing files separately from using
# them is the bug and not the fix.
SP=${SP:?set SP to the work directory}
LIB=$SP/iram-prefix/riscv-rtems7/esp32c3db/lib
LD=$SP/hal/components/esp_rom/esp32c3/ld
export PATH=$HOME/rtems/7/bin:$PATH

OBJS="$SP/confdefs-probe.o $SP/rtems_esp_glue.o $SP/ftm.o $SP/reg.o
      $SP/adapter.o $SP/wifi_init_rtems.o $SP/phy_init_data.o
      $SP/esp_event_rtems.o $SP/wpa_common.o $SP/romtest.o"

BLOBS="$SP/blobs/libnet80211.a $SP/blobs/libpp.a $SP/blobs/libcore.a
       $SP/blobs/libphy.a $SP/blobs/libmesh.a $SP/blobs/libespnow.a
       $SP/blobs/libsmartconfig.a $SP/blobs/libwapi.a"

SCRIPTS="$LD/esp32c3.rom.ld $LD/esp32c3.rom.api.ld $LD/esp32c3.rom.libgcc.ld
         $LD/esp32c3.rom.newlib.ld $LD/esp32c3.rom.libc.ld
         $LD/esp32c3.rom.version.ld $LD/esp32c3.rom.eco3.ld"

missing=
for f in $OBJS $BLOBS $SCRIPTS $LIB/librtemsbsp.a; do
  [ -r "$f" ] || missing="$missing $f"
done
if [ -n "$missing" ]; then
  echo "applink.sh: cannot link, missing:" >&2
  for f in $missing; do echo "  $f" >&2; done
  echo "(is SP right, and has everything been built?)" >&2
  exit 2
fi

Tflags=
for s in $SCRIPTS; do Tflags="$Tflags -Wl,-T,$s"; done

riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -B "$LIB" -qrtems \
  -o "$SP/applink.elf" $OBJS \
  -Wl,--unresolved-symbols=report-all -Wl,-Map,"$SP/applink.map" \
  ${WHOLE:+-Wl,--whole-archive} $BLOBS ${WHOLE:+-Wl,--no-whole-archive} \
  $Tflags -lm 2>&1
