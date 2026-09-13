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

# The supplicant and mbedtls, from tools/supplicant-build.sh and
# tools/mbedtls-build.sh.  Archives rather than objects, which is a decision
# and not a convenience: an archive member is extracted only if it defines a
# symbol that is still undefined, and a *weak* definition already counts as
# defined.  So while the weak esp_supplicant_init() stub was in
# rtems_esp_wifi_init.c, this link resolved against the stub and never pulled
# esp_wpa_main.o -- it exited zero, reported the same unresolved count as
# before, and linked none of the code being added.  Reading the archive count
# in the map is not enough either; the check is which definition won, so
# SUPPLICANT_CHECK below asks nm.
SUPPLICANT="$SP/wpa/libwpa_supplicant_rtems.a $SP/wpa/libmbedtls_rtems.a
            $SP/wpa/eloop_osdep.o"

SCRIPTS="$LD/esp32c3.rom.ld $LD/esp32c3.rom.api.ld $LD/esp32c3.rom.libgcc.ld
         $LD/esp32c3.rom.newlib.ld $LD/esp32c3.rom.libc.ld
         $LD/esp32c3.rom.version.ld $LD/esp32c3.rom.eco3.ld"

missing=
for f in $OBJS $SUPPLICANT $BLOBS $SCRIPTS $LIB/librtemsbsp.a; do
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
  ${WHOLE:+-Wl,--whole-archive} \
  -Wl,--start-group $BLOBS $SUPPLICANT -Wl,--end-group \
  ${WHOLE:+-Wl,--no-whole-archive} \
  $Tflags -lm 2>&1

# The group above is needed, not tidiness: the blobs want hexstr2bin and the
# crypto table from the supplicant, and the supplicant wants
# esp_wifi_*_internal from the blobs, so one left-to-right pass resolves
# whichever set comes second and not the other.
#
# What the link exiting zero does *not* prove.  ld is content with a weak
# definition, so ask which definition ended up in the image -- a weak one
# prints as W and the real one as T or D.  This is the guard the header
# describes, in the form the supplicant needs it.
#
# It only runs when there is an ELF to look at.  Without the placement from
# ld/esp32c3-wifi-sections.ld the blobs' sections overflow linkcmds.base's
# zero-length catch-all and ld writes no output, so this link still reports
# the six rtems_esp_wifi_* placement symbols and stops there.  For the closed
# version -- spliced linkcmds, an ELF, and this check reached -- use
# tools/supplicant-link.sh.
if [ -r "$SP/applink.elf" ]; then
  echo "--- which definition won:"
  riscv-rtems7-nm "$SP/applink.elf" \
    | grep -E " [TtDdWwVv] (esp_supplicant_init|g_wifi_default_wpa_crypto_funcs)$" \
    | while read -r addr type name; do
        case "$type" in
          W|V) echo "  $name: $type -- STILL THE WEAK STUB, the real one was not linked" ;;
          *)   echo "  $name: $type -- real definition" ;;
        esac
      done
fi
