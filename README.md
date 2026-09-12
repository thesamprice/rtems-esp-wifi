# rtems-esp-wifi

Glue between RTEMS and Espressif's WiFi HAL for the ESP32-C3, so that a
MicroPython script on RTEMS can join a network.

**Nothing works yet.** This repository holds the survey that decides whether
and how it can be built. Of the two blockers it first reported, one turned out
not to exist and the other has been cleared; both corrections are below with
the measurements behind them, so the next person starts from evidence rather
than from an estimate.

Kept separate from `rtems-lwip` and from the RTEMS MicroPython port on purpose:
the HAL is 300 MB of synchronised ESP-IDF and must not land in either tree.

## Where the pieces are

| what | where | note |
|---|---|---|
| HAL components | `espressif/esp-hal-3rdparty`, branch **`sync/master.c`** | the only branch still updated; everything else is deprecated by its own README |
| WiFi blobs | `espressif/esp32-wifi-lib`, `esp32c3/` | a submodule of the above at `components/esp_wifi/lib`, **not** part of it |
| TCP/IP | `rtems-lwip` | already works on `arm/xilinx_zynq_a9_qemu`; see the note on esp-lwip below |
| Python | RTEMS MicroPython port, branch `rtems` | sockets already work there — see `TheSamPrice/micropython` branch `rtems-networking` |

`sync/master.c` tracks ESP-IDF `master`. The deprecated `sync-*-release_v5.1`
and `sync/release_v5.*` branches track release lines and are frozen, which
matters: a frozen branch pins a blob ABI that will not move under you, and the
active one will.

## What the blobs actually need

Measured, not read off a header. `nm` over `libnet80211.a`, `libpp.a` and
`libcore.a` for `esp32c3`:

```
undefined symbols                    1260
of those, defined within the blobs   1097
genuinely external                    163
   compiler runtime (__*)              27   the toolchain supplies these
   libc                                11
   to be provided                     125
```

`components/esp_wifi/include/esp_private/wifi_os_adapter.h` declares **125
function pointers**, which is a tempting coincidence — but the two sets are not
the same thing, and that is the first blocker.

### Correction: there *is* a clean seam, and the first survey said otherwise

The first pass here claimed the blobs had no OS-adapter seam, on the evidence
that most external symbols were "internal globals of other ESP-IDF
components" -- `g_ic_ptr`, `pTxRx`, `wDevCtrl_ptr`, `lmacConfMib_ptr`,
`g_osi_funcs_p`, `g_chm`.

**That was wrong, and wrong in a way worth recording.** Every one of those is
an ESP32-C3 **ROM** symbol, declared in
`components/esp_rom/esp32c3/ld/esp32c3.rom.ld`. They live in the chip's mask
ROM. Nothing has to be written for them; a linker script has to be included.

The first pass went wrong for two avoidable reasons: it downloaded three of
the eight blobs, so symbols defined in the other five looked external, and it
never checked the ROM linker scripts because the sparse checkout did not
include `components/esp_rom`.

With all eight blobs and the ROM scripts present:

```
external with every blob present         157
   compiler runtime (__*)                 33   toolchain
   libc                                   16   toolchain
   to be provided                        106
      ESP32-C3 ROM                        65   include esp32c3.rom.ld
      component esp_wifi (C)              33
      component esp_phy (C)                3
      component esp_event (C)              2
      unresolved                           3   SC_EVENT WIFI_EVENT coex_pti_print
```

The three unresolved are trivial: `WIFI_EVENT` and `SC_EVENT` are
`esp_event_base_t` name globals, `coex_pti_print` a coexistence debug hook.

So the seam is exactly what the header says -- `wifi_osi_funcs_t`, reached
through the ROM's `g_osi_funcs_p` -- and the work is:

1. build the C of `esp_wifi`, `esp_phy` and `esp_event` for RTEMS, and
2. implement the 125-entry OS adapter.

That is what NuttX did; its adapter is 81 KiB of C
(`apache/nuttx` `arch/risc-v/src/esp32c3/esp_wifi_adapter.c`) with
`esp_coex_adapter.c` at 16 KiB beside it. The earlier estimate of 3-4 thousand
lines stands; what changed is its *nature*. It is an adapter, not a port of
five components' internals.

The hard half is (1), not (2): those sources are written against FreeRTOS
headers and a Kconfig-generated `sdkconfig.h`.

### Cleared: the blobs need IRAM, and the BSP now has a region for it

```
RAM .bss                                      9.8 KiB
RAM .data                                     3.6 KiB
RAM .iram  (code that must execute from RAM) 40.5 KiB
                                             --------
RAM total, three blobs                       54.0 KiB
flash .text + .rodata                       301.0 KiB
```

54 KiB against the **289 KiB** free on the part after RTEMS and ESPHome, so
**memory is not the problem** — which is worth saying plainly, because it was
the stated reason for expecting this to be infeasible. Flash is 301 KiB of a
4 MiB part.

The `.iram` line is the problem. 40.5 KiB of that code must execute from SRAM,
and on `riscv/esp32c3db` `REGION_FAST_TEXT` is aliased to
`CODE_FLASH_MAPPED` — there is no instruction-bus SRAM region at all. The
ESP32-C3 reaches its SRAM for instruction fetch through a window near
`0x4037c000` that neither the BSP nor the QEMU model defines.

That was the same blocker as rtems-esphome#42, and it is now cleared:
`patches/rtems/0009-esp32c3-iram-region.patch` in rtems-esphome adds a
`RAM_CODE` region at `0x40380000` sized by `ESP32C_IRAM_REGION_SIZE`, and
`tests/bsp-iram` shows a function linking there and running.

The BSP was the only half missing. QEMU already modelled the window --
`DRAM` is created as an alias into `IRAM` at offset `0x4000`, the same
arrangement the silicon has -- which the first pass here also got wrong.

Worth knowing before relying on it: with the old arrangement
`REGION_FAST_TEXT` aliased to flash, so the boot copy wrote to memory-mapped
flash and *any* use of `.fast_text` faulted at boot with a store access
fault. There was never a slow-but-working version.

## esp-lwip

Not needed, and worth saying why since it was the original suggestion.
`espressif/esp-lwip` is "a fork of lwIP with ESP-IDF specific patches" and
contains no `ssid`, no `esp_wifi` and no `wifi_config` — searching it for any
of them returns nothing. It is a TCP/IP stack; SSID is 802.11. `rtems-lwip`
already provides the same layer and already works.

What the WiFi side needs from the TCP/IP side is a netif: `s_netstack_free` and
`g_net80211_tx_func` in the list above are that seam.

## Order of work

1. ~~An instruction-bus SRAM region for `riscv/esp32c3db`~~ — **done**,
   rtems-esphome `db64d83`.
2. Include `esp32c3.rom.ld` so the 65 ROM symbols resolve. Cheap, and it
   should be done first because it shrinks the apparent surface by two thirds
   and stops anyone reimplementing ROM.
3. Build the C of `esp_wifi`, `esp_phy` and `esp_event` for RTEMS. **The bulk
   of the work**, because those sources are written against FreeRTOS headers
   and a generated `sdkconfig.h`.
4. The 125-entry OS adapter, modelled on NuttX's.
5. A netif joining it to `rtems-lwip` — `s_netstack_free` and
   `g_net80211_tx_func` are that seam.
6. `network.WLAN` in MicroPython, the smallest piece, since
   `extmod/modnetwork.c` and `extmod/network_lwip.c` already exist.

The Python API is the last step and the least of the work. Anyone starting at
that end will be blocked immediately.

## Reproducing the survey

`docs/survey.md` has the commands.
