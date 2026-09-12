# Step 3: building the components' C for RTEMS

The surface after each thing added, measured by `tools/` rather than
estimated:

| linked | unresolved | real work left |
|---|---:|---:|
| blobs alone | 106 | 106 |
| + ROM scripts | 56 | 42 |
| + `src/rtems_esp_glue.c` | 48 | 34 |
| + `ftm_load_calibration.c`, `esp_wifi_regulatory.c` | 22 | **6** |

The 22 includes 16 libc symbols that appear only because the probe links
`-nostdlib`. Read the last row as **6**; `tools/applink.sh`, added in step 4,
does the link properly and is the number to quote from here on. See
[step4.md](step4.md) -- the bare-`ld` probes are misleading enough that adding
a *working* file made the count go up.

## What made ESP-IDF's sources compile

Three headers, all owned by the port.

**`include/rtems-esp/sdkconfig.h`.** Every ESP-IDF source includes it. In
ESP-IDF it is generated from Kconfig; here it is written by hand and holds
three entries, each because a specific source asked:
`CONFIG_IDF_TARGET_ESP32C3`, `CONFIG_ESP_WIFI_FTM_ENABLE`,
`CONFIG_SOC_WIFI_SUPPORT_5G`.

FTM is on for a reason that is not a preference: the blobs reference the 24
`est_PHY_*_FTM_COMP_*` variables unconditionally, and with FTM off
`ftm_load_calibration.c` defines none of them. The choice is FTM on, or 24
hand-written stubs whose values would be wrong.

**`include/freertos/{FreeRTOS.h,task.h}`.** A *header* shim, not an
emulation: `TickType_t`, `TaskHandle_t`, `portMAX_DELAY`, `pdMS_TO_TICKS` and
a few constants. ESP-IDF's public headers name these in prototypes and will
not preprocess without them. No scheduler, no queues, no tasks — where the
libraries need those they get them through `wifi_osi_funcs_t`, which is the
seam Espressif designed for it.

One asymmetry worth knowing: `portTICK_PERIOD_MS` is a *call* here, because
RTEMS knows the tick rate at run time while FreeRTOS has it as a constant.
Anything using it in a static initialiser will not compile, which is better
than being silently wrong on a configuration that is not 1 kHz.

**`include/esp_netif.h`, and this one is the interesting case.** `esp_netif`
is deliberately **not** in `esp-hal-3rdparty` — the branch ships 34
components and the netif abstraction is not one, because a third-party
framework brings its own stack. NuttX has zero references to it. So the file
is the port's, and for now it declares two opaque types so that
`esp_wifi_default.h` parses. Step 5 fills it in against rtems-lwip.

## The six that are left

```
esp_wifi_init  esp_wifi_connect  esp_wifi_disconnect
esp_event_handler_register  esp_event_handler_unregister
esp_mesh_send_event_internal
```

Six symbols, and **this is where the work actually is.** `esp_wifi_init` is
ESP-IDF's `wifi_init.c`: it installs `wifi_osi_funcs_t` (120 members on the C3), starts
the PHY, reads calibration out of NVS and brings up the event loop. The
adapter table is reached through it.

So the count falling from 106 to 6 is real but should not be read as "nearly
done". What it means is that everything which was *data or a one-liner* is
finished, and what remains is the adapter -- which was always going to be the
81 KiB NuttX wrote, and is not smaller for the list being short.
