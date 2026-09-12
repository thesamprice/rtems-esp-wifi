# Step 2: include the ROM scripts

The ESP32-C3 carries much of the WiFi lower half in mask ROM. Espressif
declare it as `PROVIDE(...)` lists in
`components/esp_rom/esp32c3/ld/*.ld` — no `SECTIONS`, no `MEMORY`, so they
append to a BSP's own linker script with extra `-T` options rather than
replacing it.

## What it resolves

`tools/rom-link-probe.sh` links all eight C3 blobs whole-archive against those
scripts and nothing else, and reports what is left:

```
unresolved after including the ROM scripts   56
   libc / toolchain                          14   (only because the probe is -nostdlib)
   esp_phy FTM calibration constants         24
   logging hooks                              6
   esp_wifi / esp_event C                     6
   esp_phy critical sections and tables       4
   event base globals                         2
                                            ----
   genuinely to be written or built          42
```

For comparison, the same closure without the ROM scripts leaves **106**. The
ROM step removes 65 of them and costs seven `-T` options.

## What is left, in full

**esp_phy calibration data (24).** `est_PHY_INIT_FTM_COMP_*` and
`est_PHY_RESP_FTM_COMP_*` — fine-timing-measurement compensation tables. Data,
from `esp_phy`.

**esp_phy glue (4).** `phy_enter_critical`, `phy_exit_critical` — an interrupt
lock, which on RTEMS is `rtems_interrupt_lock_acquire`/`release`. Plus
`regdomain_table` and `regulatory_data`, which are tables.

**esp_wifi / esp_event C (6).** `esp_wifi_init`, `esp_wifi_connect`,
`esp_wifi_disconnect`, `esp_event_handler_register`,
`esp_event_handler_unregister`, `esp_mesh_send_event_internal`. These come
from the components' own C and are the reason step 3 exists.

**Logging hooks (6).** `pp_printf`, `phy_printf`, `net80211_printf`,
`mesh_printf`, `sc_printf`, `coex_pti_print`. One line each.

**Event bases (2).** `WIFI_EVENT`, `SC_EVENT` — `esp_event_base_t` name
globals.

## What this does *not* mean

The OS adapter is still the main implementation work, and it did not appear in
that list. `g_osi_funcs_p` is in ROM as a **pointer**; the 125-entry
`wifi_osi_funcs_t` it points at has to be supplied, and supplying it is
`esp_adapter.c` in ESP-IDF — written against FreeRTOS. That is the file whose
RTEMS equivalent NuttX wrote as 81 KiB of `esp_wifi_adapter.c`.

So the ROM step does not shrink the adapter. What it shrinks is the *apparent*
surface: it stops 65 symbols looking like work when they are already in
silicon, and it makes the remaining 42 a list short enough to plan against.

## Reproducing

```sh
tools/rom-link-probe.sh 2>&1 | grep -o "undefined.*" | sort -u
```

Needs the blobs in `blobs/` and a sparse checkout of `sync/master.c` including
`components/esp_rom`; `survey.md` has both.

## After `src/rtems_esp_glue.c`

The twelve symbols in that file are written against RTEMS alone and need
nothing from ESP-IDF, so they could be done first. Re-running the probe with
it linked in:

```
unresolved before the glue   56
unresolved after  the glue   48
resolved by it               10   the ten it defines, and nothing else
still to do, excluding libc  34
```

`tools/rom-link-probe-with-glue.sh` is the same probe with the object added;
`tools/build-glue.sh` compiles it. It builds with `-Wall -Wextra -Werror`.

What is left is now entirely `esp_phy` and `esp_wifi` data and C:

- **24** `est_PHY_*_FTM_COMP_*` calibration constants
- **2** `regdomain_table`, `regulatory_data`
- **6** `esp_wifi_init`, `esp_wifi_connect`, `esp_wifi_disconnect`,
  `esp_event_handler_register`, `esp_event_handler_unregister`,
  `esp_mesh_send_event_internal`
- **2** `printk` and `vsnprintf`, which are RTEMS and libc and appear only
  because the probe links `-nostdlib`

So step 3 is now a bounded list rather than "build three components": 32
symbols, of which 26 are data.
