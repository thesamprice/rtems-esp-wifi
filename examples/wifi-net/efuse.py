#!/usr/bin/env python3
"""Build an eFuse image with a known MAC, so read_mac can be checked.

QEMU's eFuse reads as all zeros unless it is given a backing file, and an
all-zero MAC makes the interesting check vacuous: "the netif reports what the
libraries reported" is 0 == 0 whether or not either side ever ran.

With a MAC planted here it is a real comparison, and it also settles something
that otherwise needs hardware -- that read_mac assembles the bytes in the right
order.  mac[0] is the MOST significant byte, which is the opposite way round
from the two eFuse words: MAC_1 holds the high 16 bits and MAC_0 the low 32.
Getting that backwards yields a valid-looking address no access point answers,
which is easy to mistake for a radio fault.

Layout: the file is a raw image of ESPEfuseRegs.blocks, which follows
pgm_data[8] and pgm_check[3] in the register map, so it starts at register
offset 0x2C.  EFUSE_RD_MAC_SPI_SYS_0_REG is at base + 0x44 and _1_REG at
base + 0x48, hence file offsets 0x18 and 0x1C.
"""

import struct
import sys

# Locally administered, and deliberately asymmetric: a byte-order mistake
# shows up as 56:34:12:7f:cf:5c rather than as something plausible.
MAC = [0x5C, 0xCF, 0x7F, 0x12, 0x34, 0x56]

BLOCKS_BASE = 0x2C
MAC0_REG = 0x44
MAC1_REG = 0x48


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "efuse.bin"

    mac1 = (MAC[0] << 8) | MAC[1]
    mac0 = (MAC[2] << 24) | (MAC[3] << 16) | (MAC[4] << 8) | MAC[5]

    image = bytearray(4096)
    struct.pack_into("<I", image, MAC0_REG - BLOCKS_BASE, mac0)
    struct.pack_into("<I", image, MAC1_REG - BLOCKS_BASE, mac1)

    with open(path, "wb") as fh:
        fh.write(image)

    print("efuse.py: planted %s" % ":".join("%02x" % b for b in MAC))
    return 0


if __name__ == "__main__":
    sys.exit(main())
