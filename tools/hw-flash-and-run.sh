#!/bin/sh
# Flash an image to a real ESP32-C3 and capture the console.
#
# Everything before this has been QEMU.  Two things about a real part are
# different and both are easy to get wrong once, at the worst moment:
#
# The image needs no esptool *format* -- the BSP builds for the ROM's direct
# boot, which finds two 0xaedb041d words at flash offset 0 and jumps to
# 0x42000008, with no esp_image header and no second-stage bootloader.  But it
# still needs esptool to be WRITTEN, at offset 0, which docs/esp32c3-bsp.md's
# "no esptool" is easy to misread as saying otherwise.
#
# There is a third case beyond the two below: flashing over plain UART with an
# external USB-to-serial adapter, which bypasses the board's own USB entirely.
# The C3's ROM bootloader accepts firmware on U0RXD/U0TXD (GPIO20/21)
# independently of the USB-Serial-JTAG peripheral, so a board whose USB does
# not enumerate at all can still be programmed:
#
#     adapter TX  -> GPIO20 (U0RXD)
#     adapter RX  -> GPIO21 (U0TXD)
#     adapter GND -> GND
#
# Hold BOOT while powering on and the ROM listens on UART instead of USB.  The
# adapter's own port is what this script then finds, and because the console is
# UART0 in that arrangement the image to flash is the one built with
# ESPRESSIF_USE_USB_CONSOLE = False -- examples/wifi-init/wifi-bsp.ini, not
# wifi-bsp-usb.ini.  Console output comes back over the same two wires.
#
# And which USB device the board presents decides whether the console works at
# all.  A board with a bridge chip (CP210x, CH34x, FTDI) speaks UART0, which is
# what ESPRESSIF_USE_USB_CONSOLE=False selects.  A board wired only to the C3's
# native USB-Serial-JTAG needs that option set to True or nothing is printed --
# and a silent board looks like a boot failure rather than a console setting.
# This reports which it found rather than assuming.
set -eu

# Use the RAW image, not the padded flash.bin the QEMU lanes use.  They are
# padded to the full 4 MiB because -drive if=mtd wants a whole device; writing
# 3 MiB of zeros to a real part just takes time.  objcopy's output is the whole
# image, because the link puts the load addresses at their flash offsets
# already -- see docs/esp32c3-bsp.md.
IMAGE=${1:?usage: hw-flash-and-run.sh <image.raw> [port]}
PORT=${2:-}
BAUD=${BAUD:-460800}
ESPTOOL=${ESPTOOL:-/Users/sprice5/src/rtems-esphome/.venv/bin/esptool}
LOG=${LOG:-hw-run.log}

[ -r "$IMAGE" ] || { echo "no such image: $IMAGE" >&2; exit 2; }
[ -x "$ESPTOOL" ] || { echo "no esptool at $ESPTOOL" >&2; exit 2; }

if [ -z "$PORT" ]; then
  PORT=$(ls /dev/cu.usbserial-* /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART* \
           /dev/cu.wchusbserial* 2>/dev/null | head -1 || true)
fi

if [ -z "$PORT" ]; then
  echo "No serial port found.  The board is not enumerating." >&2
  echo >&2
  echo "  * a charge-only USB cable is the usual cause, and looks identical" >&2
  echo "  * some boards have two USB sockets: the one marked UART, not the" >&2
  echo "    one wired straight to the C3, unless the console is set to USB" >&2
  echo "  * check with: ls /dev/cu.*" >&2
  exit 1
fi

echo "port: $PORT"

# Which kind of board, because it decides the console setting.
case "$PORT" in
  *usbmodem*)
    echo "This looks like the C3's native USB-Serial-JTAG."
    echo "That needs ESPRESSIF_USE_USB_CONSOLE = True in the BSP config;"
    echo "with it False the image prints nothing and looks dead."
    ;;
  *)
    echo "This looks like a USB-to-UART bridge, which is what"
    echo "ESPRESSIF_USE_USB_CONSOLE = False expects.  Good."
    ;;
esac

"$ESPTOOL" --chip esp32c3 --port "$PORT" chip-id

# Offset 0: direct boot, no bootloader and no partition table, so the whole
# flash is this image.  See docs/esp32c3-bsp.md.
"$ESPTOOL" --chip esp32c3 --port "$PORT" --baud "$BAUD" \
  write-flash 0x0 "$IMAGE"

echo
echo "flashed; capturing the console to $LOG (ctrl-a k to stop screen)"
echo

# The board resets out of the bootloader when esptool lets DTR/RTS go, so the
# banner is printed before a terminal can be opened by hand.  Capture rather
# than watch.
: > "$LOG"
screen -L -Logfile "$LOG" "$PORT" 115200
