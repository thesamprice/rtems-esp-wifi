# rtems-esp-wifi

Glue between RTEMS and Espressif's WiFi HAL for the ESP32-C3, so that a
MicroPython script on RTEMS can join a network.

**Nothing works yet.** This repository currently holds the survey that decides
whether and how it can be built, and the survey found two blockers that have to
be cleared first. Both are recorded below with the measurements behind them, so
the next person starts from evidence rather than from an estimate.

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

### Blocker 1: there is no clean OS-adapter seam

Most of the 125 external symbols are not OS functions. They are **internal
globals of other ESP-IDF components**:

```
g_osi_funcs_p  g_ic_ptr  g_chm  pTxRx  wDevCtrl_ptr  lmacConfMib_ptr
g_net80211_tx_func  s_netstack_free  if_ctrl_ptr  pp_wdev_funcs
est_PHY_INIT_FTM_COMP_*  (26 PHY calibration constants)
mesh_*  (12, from libmesh)
```

So "implement `wifi_osi_funcs_t` and link the blobs" does not describe the job.
The blobs are one layer of a stack whose other layers — `esp_phy`,
`esp_hw_support`, `esp_timer`, `esp_event`, NVS, and part of the WiFi upper
half — also have to come from `esp-hal-3rdparty` and be made to build on RTEMS.

That is what NuttX did, and its adapter is the right thing to read before
starting: `apache/nuttx` `arch/risc-v/src/esp32c3/esp_wifi_adapter.c` is **81
KiB of C**, with `esp_coex_adapter.c` (16 KiB) beside it and
`esp_wifi_api.c` / `esp_wifi_event_handler.c` / `esp_timer_adapter.c` in
`arch/risc-v/src/common/espressif/`. Call it 3–4 thousand lines of glue, for an
RTOS that already had the surrounding components ported.

### Blocker 2: the blobs need IRAM and the BSP has none

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

That is the same blocker as rtems-esphome#42, which wants it for a flash
driver. It has to be cleared for either.

## esp-lwip

Not needed, and worth saying why since it was the original suggestion.
`espressif/esp-lwip` is "a fork of lwIP with ESP-IDF specific patches" and
contains no `ssid`, no `esp_wifi` and no `wifi_config` — searching it for any
of them returns nothing. It is a TCP/IP stack; SSID is 802.11. `rtems-lwip`
already provides the same layer and already works.

What the WiFi side needs from the TCP/IP side is a netif: `s_netstack_free` and
`g_net80211_tx_func` in the list above are that seam.

## Order of work

1. An instruction-bus SRAM region for `riscv/esp32c3db`, and a QEMU model that
   implements the window. Blocks everything else here and rtems-esphome#42.
2. Build `esp_hw_support`, `esp_phy`, `esp_timer`, `esp_event` and NVS from
   `sync/master.c` for RTEMS. This is the bulk of it.
3. The OS adapter, modelled on NuttX's.
4. A netif joining it to `rtems-lwip`.
5. `network.WLAN` in MicroPython, which is the smallest piece — MicroPython
   already has `extmod/modnetwork.c` and `extmod/network_lwip.c`.

The Python API is the last step and the least of the work. Anyone starting at
that end will be blocked immediately.

## Reproducing the survey

`docs/survey.md` has the commands.
