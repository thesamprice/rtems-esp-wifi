# Step 4: the OS adapter

`wifi_osi_funcs_t` is the only way the WiFi libraries reach the OS. The blobs
call nothing else; they get there through the ROM's `g_osi_funcs_p`, which is
pointed at `g_wifi_osi_funcs`. So one file carries the whole OS surface of the
port, and after it there is nothing left on the OS side.

`src/rtems_wifi_os_adapter.c` is that file.

## What the 120 members are

The struct has 130 members in the header; 120 survive the ESP32-C3
preprocessing, the rest being guarded to other targets. Of the 120:

| | count |
|---|---:|
| implemented on RTEMS primitives | 50 |
| waiting on a component not built yet | 68 |
| `_version`, `_magic` | 2 |

The 68 break down as 23 coexistence, 12 NVS, 10 PHY and clocks, 6 event
groups, 5 ETS timers, 3 logging and 2 power management. None of them is
blocked on RTEMS; each is blocked on an ESP-IDF component this port has not
brought in.

## Why the 68 are named failures

Each reports its own name once and returns an error, rather than returning
zero quietly:

```
rtems-esp-wifi: _coex_init needs esp_coex, which is not built yet
```

An empty stub that returns zero is worse than it looks. ESP-IDF's reaction to
a missing OS primitive is not an error path — it is a queue that never
delivers, a semaphore that is always available, a calibration read that
returns zeroes. Those present as the WiFi stack failing somewhere unrelated,
which is exactly the plausible-but-wrong failure this port has already been
caught by more than once. A line naming the function is the difference between
an afternoon and a week.

## Three implementations that are not straight-through

**Priorities are inverted.** ESP-IDF numbers priorities with higher meaning
more urgent; RTEMS numbers them the other way. Passing the number through
would run the WiFi task at the *lowest* priority in the system, which presents
as dropped packets under load rather than as an error.

**Queues have to carry the item size.** RTEMS message queues fix it at create
time and ESP-IDF supplies it per-send, so the `rtems_id` alone is not enough
and the handle is a small wrapper struct holding both.

**`get_free_heap_size` is the sum of the free blocks**, via
`malloc_free_space()`. A large allocation can fail while this reports plenty.
The WiFi stack uses it for reporting and for the low-memory decisions in the
RX path, both of which tolerate that; anything that needs "can I allocate N"
must not use it.

## The table is checked, not just compiled

Designated initialisers throughout. A positional list over 120 function
pointers that drifted by one entry would call the wrong function with the
wrong arguments and fail somewhere unrelated; with named fields, a member
renamed upstream is a compile error.

A member *added* upstream is not, though — the compiler accepts a missing
designated initialiser and leaves a null pointer in the table. So:

```sh
tools/table-check.py            # diffs the preprocessed struct against the table
```

It reports missing, extra and duplicate initialisers. Both mutations fail it:

```
$ sed '/\._task_delay = /d' src/rtems_wifi_os_adapter.c > mutant.c
$ tools/table-check.py mutant.c
members 120  initialised 119  missing ['_task_delay']  extra -  dup -   # rc=1
```

## Measuring the surface properly

The earlier probes used bare `ld` with no libc, no RTEMS archives and no
linker script. That was fine while nothing being linked called RTEMS, and
stopped being fine here: every `rtems_*` and libc symbol the adapter itself
calls reported as undefined, so **adding a working file made the count go from
22 to 50.**

`tools/applink.sh` does a real `gcc` link — the BSP's `start.o` and
`linkcmds`, `tools/confdefs-probe.o` for what `<rtems/confdefs.h>` generates,
`-lm`, the ROM scripts, the blobs. It reports seven:

```
esp_wifi_init  esp_wifi_connect  esp_wifi_disconnect
esp_event_handler_register  esp_event_handler_unregister
esp_mesh_send_event_internal
hexstr2bin
```

The six `esp_*` are unchanged from step 3: ESP-IDF's own `wifi_init.c` and
event loop, which is step 5. `hexstr2bin` is in wpa_supplicant's
`src/utils/common.c` — a third source to add, not new code.

Nothing in the OS surface is outstanding.

## The section overflow is real, not a probe artifact

`applink.sh` overflows `UNEXPECTED_SECTIONS` by 93 KiB. This was first written
down here as an artifact of linking the blobs `--whole-archive`. That was
wrong: linking them the way a real build does, pulled on demand, still
overflows by 93 KiB. The blobs have section names the BSP's `linkcmds` does not
place, and `linkcmds` sweeps what it does not recognise into
`UNEXPECTED_SECTIONS` so it surfaces as an error rather than landing somewhere
quietly.

From the link map, 1549 input sections and 89.2 KiB before alignment:

| family | size | has to live |
|---|---:|---|
| `.wifi0iram.*` | 11.7 KiB | IRAM |
| `.wifislprxiram.*` | 8.8 KiB | IRAM |
| `.wifiextrairam.*` | 6.5 KiB | IRAM |
| `.wifirxiram.*` | 5.7 KiB | IRAM |
| `.wifislpiram.*` | 5.4 KiB | IRAM |
| `.iram1`, `.iram1.*` | 5.8 KiB | IRAM |
| `.wifiorslpiram.*` | 0.1 KiB | IRAM |
| `.rodata_wlog_*` | 44.8 KiB | flash |
| `.dram1.*` | 0.5 KiB | DRAM |

So 44.0 KiB must be in internal SRAM; the other 44.8 KiB is log strings that
belong in flash and were never a constraint.

The IRAM half is what `ESP32C_IRAM_REGION_SIZE` and `REGION_FAST_TEXT` were
added for in step 1, and it sets the size: at least `0xB000`, where
`config_esp32c3db_iram.ini` sets `0x2000` because that was what the test
proving the region works needed.

A bigger part does not avoid it. `SOC_SPIRAM_SUPPORTED` is not defined for the
C3 -- it has no external RAM interface -- and on the parts that do have PSRAM
this code still could not go there: these sections exist because the code runs
while the flash cache is unavailable, during a flash write and in interrupt
handlers, and PSRAM is reached through the same MSPI controller and is
unavailable at the same moments.
