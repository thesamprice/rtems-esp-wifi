#!/bin/sh
# Wait for an ESP32-C3 to appear, flash it, and open the console.
#
#   tools/flash-c3.sh              # USB-console image (SuperMini, native USB)
#   tools/flash-c3.sh uart         # UART-console image (bridge chip, or an
#                                  # external USB-serial adapter on GPIO20/21)
#
# Run it, THEN do the bootloader sequence: hold BOOT, tap RST, release BOOT.
# It polls four times a second and flashes within a quarter second of a port
# appearing, which matters -- a C3 in this state leaves bootloader mode if
# nothing is written to it, and the window is short enough that doing it by
# hand loses the race.
set -eu

SP=${SP:-/private/tmp/claude-501/-Users-sprice5-src-rtems-builder/9e0245b3-c6f3-4565-81ab-2ab4cf32d4af/scratchpad}
ESPTOOL=${ESPTOOL:-/Users/sprice5/src/rtems-esphome/.venv/bin/esptool}
SECONDS_TO_WAIT=${SECONDS_TO_WAIT:-300}

case "${1:-usb}" in
  uart) IMAGE=$SP/wifi-init-out/flash.raw;     WHICH="UART console" ;;
  usb)  IMAGE=$SP/wifi-init-usb-out/flash.raw; WHICH="USB console" ;;
  *)    IMAGE=$1;                              WHICH="$1" ;;
esac

[ -r "$IMAGE" ] || { echo "no image at $IMAGE" >&2; exit 2; }
[ -x "$ESPTOOL" ] || { echo "no esptool at $ESPTOOL" >&2; exit 2; }

echo "image: $IMAGE  ($WHICH)"
echo "waiting up to ${SECONDS_TO_WAIT}s -- do the sequence now:"
echo "    hold BOOT, tap RST, release BOOT"
echo

n=$(( SECONDS_TO_WAIT * 4 ))
i=0
while [ "$i" -lt "$n" ]; do
  port=$(ls /dev/cu.usbmodem* /dev/cu.usbserial-* /dev/cu.wchusbserial* \
            /dev/cu.SLAB_USBtoUART* 2>/dev/null | head -1 || true)
  if [ -n "$port" ]; then
    echo "port: $port"
    if "$ESPTOOL" --chip esp32c3 --port "$port" --baud 460800 \
         write-flash 0x0 "$IMAGE"; then
      echo
      echo "flashed.  opening the console -- ctrl-a then k to quit screen."
      sleep 1
      exec screen "$port" 115200
    fi
    echo "esptool did not take; still polling"
  fi
  sleep 0.25
  i=$(( i + 1 ))
done

echo "no port appeared in ${SECONDS_TO_WAIT}s." >&2
echo >&2
echo "If it never enumerates, the ROM also accepts firmware over plain UART on" >&2
echo "GPIO20/21, independently of the USB peripheral -- see the wiring note in" >&2
echo "tools/hw-flash-and-run.sh.  Then run: tools/flash-c3.sh uart" >&2
exit 1
