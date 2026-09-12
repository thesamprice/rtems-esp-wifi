#!/bin/sh
# Build the WiFi init example into a real esp32c3db image and run it in QEMU.
#
# The BSP needs ESP32C_IRAM_REGION_SIZE large enough for the blobs' 44 KiB of
# IRAM sections; the prefix this points at was configured with 0xC000.
set -eu
SP=${SP:?set SP to the work directory}
GLUE=$SP/glue
LIB=$SP/wifi-prefix/riscv-rtems7/esp32c3db/lib
HAL=$SP/hal/components
LD=$HAL/esp_rom/esp32c3/ld
QEMU=${QEMU:-/Users/sprice5/src/rtems-esphome/src/esp-qemu/build/qemu-system-riscv32}
export PATH=$HOME/rtems/7/bin:$PATH
OUT=$SP/wifi-init-out; mkdir -p "$OUT/ldir"

# One fully expanded linker script, generated here.
#
# The BSP's linkcmds ends with "INCLUDE linkcmds.base", and the WiFi sections
# have to be placed inside that file's SECTIONS block, above its
# .unexpected_sections catch-all.  Three shortcuts were tried and each failed
# quietly, which is why this does it the explicit way:
#
#   -T fragment, alongside -qrtems's own -T linkcmds: ld reads the fragment
#     first, before the MEMORY block exists, and warns "region RAM_CODE not
#     declared" for every placement.
#   -dT fragment: ld ignores a default script whenever any -T is given, so it
#     does nothing at all and the only symptom is the original overflow.
#   ld's INSERT BEFORE: augments the *default* script, so it cannot name a
#     section defined in the same file -- "`.unexpected_sections' not found for
#     insert".
#   a spliced linkcmds.base on ld's -L path: ld resolves INCLUDE relative to
#     the including script's directory, so the installed one still wins.
#
# The splice point is above .bss rather than above the .unexpected_sections
# catch-all at the end.  Anywhere above the catch-all claims the input sections
# correctly, but .work (NOLOAD) sizes itself to whatever is left of RAM, so a
# .wifi_dram placed after it overflows RAM by exactly its own 496 bytes.
#
# So: expand the INCLUDE, splice the placement in, and write the result as a
# file named linkcmds in its own directory which is put AHEAD of the BSP's on
# gcc's -B path.  -qrtems resolves its own "-T linkcmds" through that search,
# so it finds this one, and everything else -- start.o, crtbegin, the archive
# group -- still comes from the BSP directory behind it.
#
# -qrtems has to stay.  Hand-rolling its options instead (explicit archive
# group, no -qrtems) boots into the same 0xABCD0002 root-mount failure even
# with the BSP's own unmodified linkcmds, so that route is a dead end that
# looks like a linker-script bug and is not one.
mkdir -p "$OUT/ldir"
awk -v base="$LIB/linkcmds.base" -v frag="$GLUE/ld/esp32c3-wifi-sections.ld" '
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
' "$LIB/linkcmds" > "$OUT/ldir/linkcmds"

# Both halves have to be there.  Every one of the failures above produced a
# script that looked fine and placed nothing.
grep -q 'wifi_iram' "$OUT/ldir/linkcmds" || {
  echo "build-and-run.sh: the WiFi placement did not splice in" >&2; exit 2; }
grep -q 'unexpected_sections' "$OUT/ldir/linkcmds" || {
  echo "build-and-run.sh: linkcmds.base did not expand" >&2; exit 2; }

riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" -B "$OUT/ldir" -B "$LIB" -qrtems \
  -I $GLUE/include -I $GLUE/include/rtems-esp \
  -I $HAL/esp_wifi/include -I $HAL/esp_common/include -I $HAL/esp_event/include \
  -I $HAL/log/include -I $HAL/esp_rom/include -I $HAL/soc/include \
  -I $HAL/soc/esp32c3/include -I $HAL/esp_hw_support/include \
  -I $HAL/esp_timer/include -I $HAL/esp_phy/include \
  -o "$OUT/wifi-init.exe" \
  "$GLUE/examples/wifi-init/init.c" \
  $SP/rtems_esp_glue.o $SP/ftm.o $SP/reg.o $SP/adapter.o \
  $SP/wifi_init_rtems.o $SP/phy_init_data.o $SP/esp_event_rtems.o \
  $SP/wpa_common.o \
  $SP/blobs/libnet80211.a $SP/blobs/libpp.a $SP/blobs/libcore.a \
  $SP/blobs/libphy.a $SP/blobs/libmesh.a $SP/blobs/libespnow.a \
  $SP/blobs/libsmartconfig.a $SP/blobs/libwapi.a \
  $SP/blobs/libnet80211.a $SP/blobs/libpp.a $SP/blobs/libcore.a $SP/blobs/libphy.a \
  `# The WiFi and PHY ROM symbols, and nothing else.` \
  `#` \
  `# Deliberately NOT rom.libc.ld, rom.newlib.ld or rom.libgcc.ld.  Those point` \
  `# printf, memcpy, memset and strcmp at ROM addresses, which is right for` \
  `# ESP-IDF -- it uses ROM libc to save flash -- and wrong here: an RTEMS image` \
  `# linked with them calls ROM printf instead of RTEMS' console and the boot` \
  `# dies in the root filesystem mount with 0xABCD0002 before Init runs.` \
  `#` \
  `# They are not needed either.  All the blobs want from them is memset,` \
  `# strlen, strnlen and 33 compiler-runtime helpers, and newlib and libgcc` \
  `# provide every one.  They only looked necessary in the earlier probes` \
  `# because those linked -nostdlib.` \
  -Wl,-T,$LD/esp32c3.rom.ld -Wl,-T,$LD/esp32c3.rom.api.ld \
  -Wl,-T,$LD/esp32c3.rom.version.ld -Wl,-T,$LD/esp32c3.rom.eco3.ld \
  -Wl,-Map,"$OUT/wifi-init.map" -lm

riscv-rtems7-size "$OUT/wifi-init.exe"

riscv-rtems7-objcopy -O binary "$OUT/wifi-init.exe" "$OUT/flash.raw"
dd if=/dev/zero of="$OUT/flash.bin" bs=1048576 count=4 2>/dev/null
dd if="$OUT/flash.raw" of="$OUT/flash.bin" conv=notrunc 2>/dev/null

rm -f "$OUT/run.log"
"$QEMU" -M esp32c3 -display none -monitor none -no-reboot \
  -icount shift=0,sleep=off -serial file:"$OUT/run.log" \
  -drive file="$OUT/flash.bin",if=mtd,format=raw > /dev/null 2>&1 &
qpid=$!
for _ in $(seq 1 160); do
  grep -q "END OF" "$OUT/run.log" 2>/dev/null && break
  kill -0 $qpid 2>/dev/null || break
  sleep 0.5
done
sleep 1; kill $qpid 2>/dev/null || true; wait $qpid 2>/dev/null || true
cat "$OUT/run.log"
