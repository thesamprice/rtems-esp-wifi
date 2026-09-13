#!/bin/sh
# Build lwIP, the netif and the WiFi libraries into one esp32c3db image and run
# it in QEMU.
#
# This is examples/wifi-init/build-and-run.sh with three additions: the netif
# object, -llwip, and the memory report at the end.  Everything else -- the
# generated linker script, the choice of ROM scripts, the -u for the
# supplicant -- is unchanged and the comments explaining it are left where they
# are, because each of them records a failure that was quiet.
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
OUT=$SP/wifi-net-out; mkdir -p "$OUT/ldir"

# liblwip.a is the thing this example exists to link, so its absence is worth a
# sentence rather than a page of undefined symbols.
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

# The include path the netif needs is the WiFi one plus lwIP's, and lwIP's
# arrives through -isystem on the BSP's lib/include -- the port is installed,
# so these are the real headers the library was compiled against rather than a
# source checkout's.
riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" \
  -I $GLUE/include -I $GLUE/include/rtems-esp \
  -I $HAL/esp_wifi/include -I $HAL/esp_common/include -I $HAL/esp_event/include \
  -I $HAL/log/include -I $HAL/esp_rom/include -I $HAL/soc/include \
  -I $HAL/soc/esp32c3/include -I $HAL/esp_hw_support/include \
  -I $HAL/esp_timer/include -I $HAL/esp_phy/include \
  -Wall -Wextra -Werror -c -o "$OUT/rtems_esp_netif.o" \
  "$GLUE/src/rtems_esp_netif.c"

riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" -B "$OUT/ldir" -B "$LIB" -qrtems \
  -I $GLUE/include -I $GLUE/include/rtems-esp \
  -I $HAL/esp_wifi/include -I $HAL/esp_common/include -I $HAL/esp_event/include \
  -I $HAL/log/include -I $HAL/esp_rom/include -I $HAL/soc/include \
  -I $HAL/soc/esp32c3/include -I $HAL/esp_hw_support/include \
  -I $HAL/esp_timer/include -I $HAL/esp_phy/include \
  -o "$OUT/wifi-net.exe" \
  "$GLUE/examples/wifi-net/init.c" \
  `# The netif goes on the link line as an object, not into an archive.` \
  `#` \
  `# liblwip.a's netstart.o defines esp32c3_netif_add WEAKLY, and a weak` \
  `# definition satisfies a reference, so ld would never go looking for a` \
  `# strong one in an archive.  The supplicant cost an hour to exactly that.` \
  `# An object named on the command line is always loaded, so the strong` \
  `# definition is present before liblwip is searched and wins.  The nm check` \
  `# after the link is what proves it rather than assuming it.` \
  "$OUT/rtems_esp_netif.o" \
  $SP/rtems_esp_glue.o $SP/ftm.o $SP/reg.o $SP/adapter.o \
  $SP/wifi_init_rtems.o $SP/phy_init_data.o $SP/esp_event_rtems.o \
  $SP/wpa_common.o \
  $SP/blobs/libnet80211.a $SP/blobs/libpp.a $SP/blobs/libcore.a \
  $SP/blobs/libphy.a $SP/blobs/libmesh.a $SP/blobs/libespnow.a \
  $SP/blobs/libsmartconfig.a $SP/blobs/libwapi.a \
  `# The supplicant and its crypto.` \
  `#` \
  `# -u is required, not decoration.  src/rtems_esp_wifi_init.c defines` \
  `# esp_supplicant_init and g_wifi_default_wpa_crypto_funcs WEAKLY so that a` \
  `# build without the supplicant still links.  A weak definition satisfies the` \
  `# reference, so ld never searches the archive for the strong one and the` \
  `# stub silently wins -- the link succeeds, reports zero undefined symbols,` \
  `# and produces an image that cannot do WPA2.  nm showing "W" rather than "T"` \
  `# is the only outward sign.  -u makes the symbol undefined up front, which` \
  `# forces the archive member in.` \
  -Wl,-u,esp_supplicant_init -Wl,-u,g_wifi_default_wpa_crypto_funcs \
  -Wl,--start-group $SP/libwpa.a $SP/libmbedtls.a -Wl,--end-group \
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
  -llwip \
  -Wl,-Map,"$OUT/wifi-net.map" -lm

# The strong definition won.  "W" here would mean liblwip's do-nothing default
# is in the image, which links cleanly, runs cleanly, and gives the application
# no interface -- exactly the failure mode -u exists to prevent above.
kind=$(riscv-rtems7-nm "$OUT/wifi-net.exe" | awk '$3 == "esp32c3_netif_add" { print $2 }')
[ "$kind" = "T" ] || {
  echo "build-and-run.sh: esp32c3_netif_add is '$kind', not 'T'; the weak" >&2
  echo "build-and-run.sh: default in liblwip won and there is no interface" >&2
  exit 2; }

# And lwIP is really in the image rather than merely on the command line.
riscv-rtems7-nm "$OUT/wifi-net.exe" | grep -q " T tcpip_input$" || {
  echo "build-and-run.sh: tcpip_input is not in the image" >&2; exit 2; }

echo
echo "=== memory ==="
riscv-rtems7-size "$OUT/wifi-net.exe"

# The number rtems-esphome#108 asks for.  lwIP's pbuf pool and MEM_SIZE were
# chosen against "218 KiB free", measured on an image that had no lwIP in it,
# so the only honest version of that measurement is this one: what is left for
# the RTEMS workspace and the C heap once everything is linked.
#
# CONFIGURE_UNIFIED_WORK_AREAS makes .work both, and the linker script sizes it
# to whatever remains of the RAM region, so its extent is the free memory.
#
# The arithmetic is the shell's rather than awk's: the awk on macOS is the one
# true awk, which has no strtonum, and its absence is a runtime error in the
# middle of the report rather than anything the script notices.
riscv-rtems7-nm "$OUT/wifi-net.exe" > "$OUT/symbols.txt"
wb=$(awk '$3 == "bsp_section_work_begin" { print $1 }' "$OUT/symbols.txt")
we=$(awk '$3 == "bsp_section_work_end"   { print $1 }' "$OUT/symbols.txt")
ws=$(( 0x$we - 0x$wb ))
echo "work area   0x$wb .. 0x$we  $ws bytes ($(( ws / 1024 )) KiB)"

# lwIP's two large statics, named so that the report says the buffers are
# really in the image and not configured away.
for sym in memp_memory_PBUF_POOL_base ram_heap; do
  if grep -q " $sym\$" "$OUT/symbols.txt"; then
    echo "lwIP static $sym present"
  else
    echo "build-and-run.sh: lwIP static $sym is missing" >&2; exit 2
  fi
done

riscv-rtems7-size -A "$OUT/wifi-net.exe" \
  | awk '$1 ~ /^\.(bss|work|noinit|data|sbss|wifi_dram|rtemsstack)$/ {
           printf "  %-14s %8d\n", $1, $2 }'
echo

riscv-rtems7-objcopy -O binary "$OUT/wifi-net.exe" "$OUT/flash.raw"
dd if=/dev/zero of="$OUT/flash.bin" bs=1048576 count=4 2>/dev/null
dd if="$OUT/flash.raw" of="$OUT/flash.bin" conv=notrunc 2>/dev/null

rm -f "$OUT/run.log"
# An eFuse with a known MAC in it.
#
# Without one QEMU's eFuse reads as all zeros, and the check that the netif
# reports the same address the WiFi libraries do becomes 0 == 0 -- true whether
# or not either side ran.  With a MAC planted, that check is real, and so is
# the byte order: mac[0] is the most significant byte, which is the opposite
# way round from the two eFuse words, and getting it backwards gives a
# valid-looking address no access point ever answers.
#
# That was on the hardware bring-up list until this made it checkable here.
python3 "$GLUE/examples/wifi-net/efuse.py" "$OUT/efuse.bin"

"$QEMU" -M esp32c3 -display none -monitor none -no-reboot \
  -icount shift=0,sleep=off -serial file:"$OUT/run.log" \
  -drive file="$OUT/flash.bin",if=mtd,format=raw \
  -drive file="$OUT/efuse.bin",if=none,format=raw,id=efuse \
  -global driver=nvram.esp32c3.efuse,property=drive,value=efuse > /dev/null 2>&1 &
qpid=$!
for _ in $(seq 1 240); do
  grep -q "END OF" "$OUT/run.log" 2>/dev/null && break
  kill -0 $qpid 2>/dev/null || break
  sleep 0.5
done
sleep 1; kill $qpid 2>/dev/null || true; wait $qpid 2>/dev/null || true
cat "$OUT/run.log"
