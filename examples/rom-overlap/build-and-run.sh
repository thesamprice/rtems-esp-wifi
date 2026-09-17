#!/bin/sh
# Build the ROM-overlap probe (rtems-esphome#120) into an esp32c3db image.
#
# This is examples/wifi-net/build-and-run.sh with the netif and lwIP left in --
# the image is meant to be the one the port really runs, not a reduced one --
# and examples/rom-overlap/init.c in place of the wifi-net source.  Everything
# structural is unchanged and the comments explaining it are left where they
# are, because each of them records a failure that was quiet.
#
# It does NOT run QEMU.  The measurement needs an association and QEMU's
# esp32c3 has no WiFi MAC, so a QEMU run can only ever report the one failure
# that says so.  Build here, flash with tools/hw-flash-and-run.sh, read the log.
#
# Two things the prefix this points at must have been configured with:
#
#   ESP32C_IRAM_REGION_SIZE >= 0xc000, for the blobs' 44 KiB of IRAM sections.
#   ESP32C_DRAM_ROM_RESERVE_SIZE > 0,  or there is no window to fill and the
#                                      test will say so and refuse.
#
# The second is checked after the link rather than assumed; see below.
#
# Usage, all three runs on the same board:
#
#   # the measurement
#   SP=$HOME/build/c3 EXTRA_CFLAGS='-DROM_OVERLAP_SSID="your-ssid"
#     -DROM_OVERLAP_PASSWORD="your-passphrase"' \
#     src/rtems-esp-wifi/examples/rom-overlap/build-and-run.sh
#   src/rtems-esp-wifi/tools/hw-flash-and-run.sh $SP/rom-overlap-out/flash.raw
#
#   # control 1: the detector can see a write.  Must FAIL to find the window
#   # clean and must name the planted address.
#   ... EXTRA_CFLAGS='... -DROM_OVERLAP_SELF_TEST' ...
#
#   # control 2: the linker half.  Against a prefix built without the #120
#   # patch the test must refuse to fill and say the range is in the heap.
#   REQUIRE_RESERVE=0 SP=$HOME/build/c3-unpatched ...
#
# Do not believe a clean measurement until both controls have been run on the
# same board in the same session.
set -eu
SP=${SP:?set SP to the work directory}
GLUE=$SP/glue
LIB=$SP/wifi-prefix/riscv-rtems7/esp32c3db/lib
HAL=$SP/hal/components
LD=$HAL/esp_rom/esp32c3/ld
export PATH=$HOME/rtems/7/bin:$PATH
OUT=$SP/rom-overlap-out; mkdir -p "$OUT/ldir"

# Credentials and options, passed in rather than edited in.
#
# An empty password is accepted by the build and is close to useless here: an
# open access point does no four-way handshake, and the handshake is one of the
# ROM paths this exists to exercise.  Set both.
EXTRA_CFLAGS=${EXTRA_CFLAGS:-}

[ -f "$LIB/liblwip.a" ] || {
  echo "build-and-run.sh: no liblwip.a in $LIB; build rtems-lwip for" >&2
  echo "build-and-run.sh: riscv/esp32c3db first" >&2; exit 2; }

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
#     section defined in the same file.
#   a spliced linkcmds.base on ld's -L path: ld resolves INCLUDE relative to
#     the including script's directory, so the installed one still wins.
#
# The splice point is above .bss rather than above the catch-all at the end,
# because .work (NOLOAD) sizes itself to whatever is left of RAM and a
# .wifi_dram placed after it overflows RAM by exactly its own 496 bytes.
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

grep -q 'wifi_iram' "$OUT/ldir/linkcmds" || {
  echo "build-and-run.sh: the WiFi placement did not splice in" >&2; exit 2; }
grep -q 'unexpected_sections' "$OUT/ldir/linkcmds" || {
  echo "build-and-run.sh: linkcmds.base did not expand" >&2; exit 2; }

riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" \
  -I $GLUE/include -I $GLUE/include/rtems-esp \
  -I $HAL/esp_wifi/include -I $HAL/esp_common/include -I $HAL/esp_event/include \
  -I $HAL/log/include -I $HAL/esp_rom/include -I $HAL/soc/include \
  -I $HAL/soc/esp32c3/include -I $HAL/esp_hw_support/include \
  -I $HAL/esp_timer/include -I $HAL/esp_phy/include \
  -Wall -Wextra -Werror -c -o "$OUT/rtems_esp_netif.o" \
  "$GLUE/src/rtems_esp_netif.c"

# shellcheck disable=SC2086  # EXTRA_CFLAGS is meant to word-split.
riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" \
  -B "$OUT/ldir" -B "$LIB" -qrtems \
  $EXTRA_CFLAGS \
  -I $GLUE/include -I $GLUE/include/rtems-esp \
  -I $HAL/esp_wifi/include -I $HAL/esp_common/include -I $HAL/esp_event/include \
  -I $HAL/log/include -I $HAL/esp_rom/include -I $HAL/soc/include \
  -I $HAL/soc/esp32c3/include -I $HAL/esp_hw_support/include \
  -I $HAL/esp_timer/include -I $HAL/esp_phy/include \
  -o "$OUT/rom-overlap.exe" \
  "$GLUE/examples/rom-overlap/init.c" \
  `# The netif goes on the link line as an object, not into an archive:` \
  `# liblwip.a defines esp32c3_netif_add WEAKLY and a weak definition` \
  `# satisfies a reference, so ld would never search the archive.` \
  "$OUT/rtems_esp_netif.o" \
  $SP/rtems_esp_glue.o $SP/ftm.o $SP/reg.o $SP/adapter.o \
  $SP/wifi_init_rtems.o $SP/phy_init_data.o $SP/esp_event_rtems.o \
  $SP/wpa_common.o \
  $SP/blobs/libnet80211.a $SP/blobs/libpp.a $SP/blobs/libcore.a \
  $SP/blobs/libphy.a $SP/blobs/libmesh.a $SP/blobs/libespnow.a \
  $SP/blobs/libsmartconfig.a $SP/blobs/libwapi.a \
  `# -u is required, not decoration.  src/rtems_esp_wifi_init.c defines` \
  `# esp_supplicant_init and g_wifi_default_wpa_crypto_funcs WEAKLY so that` \
  `# a build without the supplicant still links -- and then cannot do WPA2,` \
  `# with nm showing "W" rather than "T" as the only outward sign.  This` \
  `# test needs the four-way handshake to run, so it needs the real one.` \
  -Wl,-u,esp_supplicant_init -Wl,-u,g_wifi_default_wpa_crypto_funcs \
  -Wl,--start-group $SP/libwpa.a $SP/libmbedtls.a -Wl,--end-group \
  $SP/blobs/libnet80211.a $SP/blobs/libpp.a $SP/blobs/libcore.a $SP/blobs/libphy.a \
  `# The WiFi and PHY ROM symbols, and nothing else.  Deliberately NOT` \
  `# rom.libc.ld, rom.newlib.ld or rom.libgcc.ld: those point printf,` \
  `# memcpy and friends at ROM addresses, and an RTEMS image linked with` \
  `# them dies in the root filesystem mount with 0xABCD0002 before Init.` \
  -Wl,-T,$LD/esp32c3.rom.ld -Wl,-T,$LD/esp32c3.rom.api.ld \
  -Wl,-T,$LD/esp32c3.rom.version.ld -Wl,-T,$LD/esp32c3.rom.eco3.ld \
  -llwip \
  -Wl,-Map,"$OUT/rom-overlap.map" -lm

riscv-rtems7-nm "$OUT/rom-overlap.exe" > "$OUT/symbols.txt"

# The supplicant's strong definition won.  "W" means the do-nothing stub is in
# the image: it links, it runs, and it cannot do WPA2 -- so the run would never
# associate and the test would report exactly that, blaming the access point.
kind=$(awk '$3 == "esp_supplicant_init" { print $2 }' "$OUT/symbols.txt")
[ "$kind" = "T" ] || {
  echo "build-and-run.sh: esp_supplicant_init is '$kind', not 'T'; the weak" >&2
  echo "build-and-run.sh: stub won and this image cannot do WPA2" >&2; exit 2; }

# THE CHECK THAT MAKES THE MEASUREMENT REAL.
#
# Without a ROM reserve there is no window: the range the test wants to fill is
# inside the heap, and it will refuse rather than corrupt it.  That refusal is
# a legitimate result -- it is the negative control for the linker half -- but
# it must be something asked for, not something arrived at by flashing the
# wrong prefix.  So this reports what the image actually has rather than
# assuming, and only stops if REQUIRE_RESERVE says the reserve was the point.
reserve=$(awk '$3 == "esp32c_dram_rom_reserve_begin" { print $1 }' "$OUT/symbols.txt")
work=$(awk '$3 == "bsp_section_work_end" { print $1 }' "$OUT/symbols.txt")
echo
if [ -z "$reserve" ]; then
  echo "ROM reserve  none -- this BSP predates the #120 fix"
  echo "work area ends at 0x$work"
  if [ "${REQUIRE_RESERVE:-1}" = 1 ]; then
    echo "build-and-run.sh: no esp32c_dram_rom_reserve_begin in the image." >&2
    echo "build-and-run.sh: rebuild the BSP with patches/rtems/0013 applied," >&2
    echo "build-and-run.sh: or set REQUIRE_RESERVE=0 to build the control." >&2
    exit 2
  fi
else
  echo "ROM reserve  0x$reserve .. 0x3fcd0000  ($(( 0x3fcd0000 - 0x$reserve )) bytes)"
  echo "work area ends at 0x$work"
fi

echo
echo "=== memory ==="
riscv-rtems7-size "$OUT/rom-overlap.exe"

# What the ceiling costs, in the only unit that matters here.  .work is sized
# by the linker to whatever is left of RAM, so its extent is the free memory.
wb=$(awk '$3 == "bsp_section_work_begin" { print $1 }' "$OUT/symbols.txt")
ws=$(( 0x$work - 0x$wb ))
echo "work area   0x$wb .. 0x$work  $ws bytes ($(( ws / 1024 )) KiB)"

riscv-rtems7-objcopy -O binary "$OUT/rom-overlap.exe" "$OUT/flash.raw"
dd if=/dev/zero of="$OUT/flash.bin" bs=1048576 count=4 2>/dev/null
dd if="$OUT/flash.raw" of="$OUT/flash.bin" conv=notrunc 2>/dev/null

echo
echo "image: $OUT/flash.raw"
echo "flash: src/rtems-esp-wifi/tools/hw-flash-and-run.sh $OUT/flash.raw"
