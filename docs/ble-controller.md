# The ESP32-C3 BLE controller on RTEMS: where this stands

Handoff. Branch `ble-controller`, head `b0f0973`. Everything below was measured
on an ESP32-C3 rev 0.4 over USB-Serial-JTAG at `/dev/cu.usbmodem2101` unless it
says otherwise.

**Status: the controller initialises, the link layer never starts.**
`btdm_controller_enable()` does not return. No BLE packet has been sent or
received.

## The one-sentence version

Two tasks are blocked on two semaphores, and the thing that should give one of
them is `r_btdm_task_post`, which resolves to a ROM version that contains no
`_semphr_give` at all — where stock resolves it to the blob's replacement,
which does.

## Why this is much smaller than it looks

`libbtdm_app.a` has 1369 undefined symbols. 1368 are satisfied by the part's
**own ROM** — `esp32c3.rom.bt_funcs.ld` names most of the Riviera Waves link
layer — plus `libbtbb.a` and `libphy.a`, both already in `esp-phy-lib`. The one
left is `ets_delay_us`.

The stack is in silicon. What has to be supplied is a runtime contract:
`osi_funcs_t`, 61 function pointers, filled against RTEMS instead of FreeRTOS.
The primitives came from `rtems_wifi_os_adapter.c`.

## What works, verified on hardware

```
rtems-esp-bt: copied 32884 bytes of IRAM and 300 of DRAM
rtems-esp-bt: powering the modem domain...
rtems-esp-bt: powering the Bluetooth domain...
rtems-esp-bt: registering the OS adapter (61 entries)...
rtems-esp-bt: controller version 51d9dfd
rtems-esp-bt: calibrating for MAC ac:a7:04:da:38:28
rtems-esp-bt: register_chipv7_phy( PHY_RF_CAL_FULL )...
rtems-esp-bt: no saved calibration, so the PHY calibrated fully
rtems-esp-bt: bt_bb_v2_init_cmplx()...
rtems-esp-bt: btdm_controller_init()...          <- takes 10 s, see below
rtems-esp-bt: controller initialised
rtems-esp-bt: btdm_controller_enable( BLE )...   <- never returns
```

Four things had to be right before the PHY would calibrate rather than spin
printing carriage returns forever, and each looked like a hang rather than an
error:

1. **The shared modem domain is not the Bluetooth domain.**
   `bsp_esp32_bt_enable()` powers the latter. `register_chipv7_phy()` needs the
   front end, AGC and baseband, which live in the domain `RTC_CNTL` calls WiFi.
   ESP-IDF's name for bringing it up, `esp_wifi_bt_power_domain_on()`, is called
   from the Bluetooth path too.
2. **The internal analog I2C master has to be opened** (`ANA_CONFIG`,
   `ANA_CONFIG2`), or the calibration has a digital block to talk to and no
   analog one behind it.
3. **The CPU has to be on the BBPLL at 160 MHz.** A direct-boot image has no
   second stage, so nothing ran `rtc_clk_init()` and the core is still on the
   crystal.
4. **The blob's `.iram1` sections have to be copied.**
   `bsp_start_copy_sections()` only knows the BSP's own `.fast_text`; without
   the copy the first call into an IRAM function takes an illegal instruction.

## The deadlock, exactly

Traced by instrumenting the adapter **on entry as well as on return** — the
earlier version printed only on return, so a task blocked inside a call printed
nothing, and that produced two wrong conclusions that are now retracted.

```
semphr 0 created max 1 init 0
semphr 1 created max 1 init 0
semphr 2 created max 1 init 1
queue_send ENTER sig=7 ; queue_send EXIT sc=0
semphr_take ENTER sem 0 (10000 ms)      task 0x0a010001   <- init handshake
controller task 0x0a010003 running, entry 0x40380322
semphr_take ENTER sem 1 (4294967295 ms) task 0x0a010003   <- blocks forever
semphr_take(10000 ms) by task 0x0a010001 = 0 after 9993 ms
queue_send ENTER sig=9 ; queue_send EXIT sc=0
semphr_take ENTER sem 0 (4294967295 ms) task 0x0a010001   <- THE HANG
```

`rtems_cpu_usage_report()` at that point: **IDLE 98.2 %**, controller task
1.6 ms of CPU in thirteen seconds. Nothing spins; the system sleeps.
`_Thread_Get()` says the controller task is `STATES_WAITING_FOR_SEMAPHORE`.

## The blob's side, from disassembly

Addresses are from the current link; `full.dis` and `rom.dis` are regenerable,
see below.

```
btdm_controller_enable   0x42002bb2
  r_sdk_config_get_opts, check mode matches
  plf[+40]  r_btdm_task_post( sig 9 )
  osi[+52]  _semphr_take( g_rw_init_sem, forever )   0x42002bf6   <- the hang
  return 0

btdm_controller_init     0x42002c36
  osi[+100] _task_create( btdm_controller_task )   must return exactly 1
  plf[+40]  r_btdm_task_post( sig 7 )
  osi[+52]  _semphr_take( g_rw_init_sem, 10000 ms )  0x42003200
            return value forced to 0 *before* the call -- a timeout reports
            success, which is why init takes exactly 10 s and still "works"

btdm_controller_task     0x40380322   (blob IRAM)
  loop: osi[+52] _semphr_take( env->sem, forever )     0x40380374
        if it returns 0 the task RETURNS -- so _semphr_take must return
        non-zero on success, which this port does
        osi[+92] _queue_recv( g_rw_schd_queue, &msg, 0 )   non-blocking drain
        sig 7  -> rw_pre_main -> r_intc_init -> osi[+8] _interrupt_alloc(
                  source 8, r_rwbtdm_isr_wrapper )   <- the only interrupt
                  stock routes, and the only place _interrupt_alloc is called
        sig 9  -> bt_bb_v2_init_cmplx, btdm_controller_on_reset,
                  r_intc_enable, then _semphr_give( g_rw_init_sem ) 0x40380428
```

So sig 7 is never dequeued, `rw_pre_main` never runs, `_interrupt_alloc` is
never called, and the sig-9 handler that would release `g_rw_init_sem` is in
the same loop. Everything follows from the controller task never being woken.

**What should wake it.** `r_btdm_task_post` is meant to `_queue_send` and then
`_semphr_give( env->sem )`. The blob's `r_btdm_task_post_impl` at `0x42037000`
does exactly that, unconditionally on both branches (`0x420370b8`). But the
table entry used at runtime points at the **ROM** version, disassembled from
`esp32c3-rom.bin` at `0x400314ec` via the jump table at `0x40000c14`, and that
one does `_queue_send` and returns — **no `_semphr_give` at any address in it**.

## The single remaining difference from stock

```
r_plf_funcs_p[40]    stock  0x42055de6  = r_btdm_task_post_hack   (blob, flash)
                     here   0x40000c14  = r_btdm_task_post        (ROM)
```

Everything upstream of that word now matches stock by measurement.

## Ruled out — do not re-investigate

| Candidate | How it was eliminated |
|---|---|
| A different controller blob | stock's `libbtdm_app.a` is **byte-identical**: same sha256 `2c503e2f…`, 1461 symbols each, zero diff, both report `51d9dfd` |
| `osi_funcs` layout | parsed both structs and diffed: identical order and count. Independently, the disassembly's offsets (`_interrupt_alloc` 8, `_semphr_take` 52, `_queue_recv` 92, `_task_create` 100) all match |
| OSI magic / version | `btdm_osi_funcs_register()` accepts the table |
| Controller config values | now stock's 128 `CONFIG_BT_*` defines verbatim, extracted from stock's generated `sdkconfig.h`. The hand-written set had 26, two with wrong values, and wrongly set `CONFIG_BT_BLE_50_FEATURES_SUPPORTED` |
| Modem clocks | `CLK_EN` now `0xffffffff` (NuttX writes `UINT32_MAX`) |
| Power / isolation | handed back to automatic before enable: `DIG_PWC 00000000`, `DIG_ISO 00000080`, byte-identical to stock |
| Low-power clock | tried main XTAL ÷40; NuttX does not set it at all when sleep is off, and it is now not set |
| PHY ordering | tried both before and after `btdm_controller_init()`; ESP-IDF v5.5 and NuttX disagree, so it is not load-bearing |
| Task priority | the mapping was inverted (FreeRTOS counts up, RTEMS down). Fixed; not the cause |
| Controller memory init | on this chip `btdm_controller_mem_init()` is only `btdm_controller_rom_data_init()`, already called |
| Read-only adapter table | ESP-IDF mallocs a RAM copy; this now does too. Stock's `r_osi_funcs_p` is RAM, this port's was flash rodata where writes are silently dropped |
| Blanket ROM-symbol filtering | tried removing the 936 assignments the blob also defines; **withdrawn** — it moved `btdm_controller_rom_data_init` to the blob where stock uses the ROM's at `0x400008ec` |
| Dropping the eco3 overlays | tried; reverted. This part is rev 0.4 so they apply, and without them eight `ip_funcs` entries are undefined |
| The radio / antenna | a WiFi scan on the same board finds 10 APs, repeatably |
| "No advertisers heard" as a test | **stock finds zero too** in this room. Not a valid pass criterion here |

## Two retracted claims

Both came from instrumenting only on return:

- "The controller task makes no adapter calls." It does — it reaches its main
  loop and blocks in `_semphr_take`.
- "Only two adapter entries are ever called." Same artefact.

## Reference implementations

- **NuttX**, `arch/risc-v/src/esp32c3-legacy/esp32c3_ble_adapter.c` — the other
  non-FreeRTOS port of this same blob, and therefore the better guide to what
  the contract *requires* versus what one RTOS happens to do. Its
  `esp32c3_bt_controller_init()` is the closest thing to a specification.
  A local copy is in the scratchpad; re-fetch from `apache/nuttx` master.
- **ESP-IDF**, `components/bt/controller/esp32c3/bt.c` in
  `esp-hal-3rdparty`, and the working build under
  `scratchpad/ble/stock/.esphome/build/`.
- Not yet consulted: the Rust `esp-wifi` crate, which also drives this blob
  without FreeRTOS and is a third independent reading of the contract.

## How to build, flash and run

```sh
SP=<scratchpad>/ble
sh $SP/build.sh glue     # compile rtems_esp_bt.c
sh $SP/build.sh app      # link ble-scan.exe, objcopy to ble-scan.raw
<venv>/bin/esptool --chip esp32c3 --port /dev/cu.usbmodem2101 --baud 460800 \
    --before default-reset --after hard-reset write-flash 0 $SP/ble-scan.raw
```

Then read the console with pyserial at 115200, toggling RTS to reset. The blob
is fetched, not vendored: `espressif/esp32c3-bt-lib` at `0a08c4b`, expected at
`$SP/esp32c3-bt-lib`.

The stock A-side rebuilds with
`<venv>/bin/esphome compile $SP/stock/ble-probe.yaml` and flashes at the four
standard offsets. Its `interval:` lambda dumps the same registers this port
prints, which is what makes the A/B possible.

## Measurement apparatus already in place

- Every `osi_funcs` entry that matters logs **on entry**, with the calling task
  id, so a blocked call is visible.
- `rtems_esp_bt_calls` is a bitmask of which entries have been reached.
- The example's `Monitor` task prints, every three seconds: the bitmask, the
  interrupt-matrix entries for sources 4–10, `rtems_cpu_usage_report()`, and the
  controller task's RTEMS state and thread-queue name.
- `rtems_esp_bt_controller_init()` prints `r_plf_funcs_p`, `r_osi_funcs_p`,
  `plf[40]` and `plf[248]` after `btdm_controller_init()`.

## Next experiments, in the order I would try them

1. **Find what writes `r_plf_funcs_p[40]`.** With link resolution now identical
   to stock's, the same ROM `btdm_controller_rom_data_init()` produces different
   values in the two builds, so it depends on machine state at the moment of the
   call. Dump the whole RAM table in both builds and diff every slot, not just
   40 and 248 — the pattern of which slots differ will say what the patcher keyed
   on.
2. **Write the entry by hand.** After `btdm_controller_init()`, set
   `r_plf_funcs_p[10] = (uint32_t) &r_btdm_task_post_hack`. If the controller
   task then wakes, the diagnosis is confirmed even if the mechanism is not, and
   everything downstream becomes reachable for the first time. This is a hack and
   should be labelled one, but it is the fastest way to learn whether anything
   else is also wrong.
3. **`periph_module_enable( PERIPH_BT_MODULE )` / `periph_module_reset()`.**
   ESP-IDF calls both between registering the adapter and
   `btdm_controller_init()`; this port calls neither. NuttX does
   `modifyreg32( SYSTEM_WIFI_CLK_EN_REG, 0, UINT32_MAX )` instead, which is
   done — but the *reset* half is not.
4. **JTAG.** The C3 has hardware breakpoints and OpenOCD, and there is a
   `pmp-openocd.log` in the tree from earlier work. A watchpoint on
   `r_plf_funcs_p[10]` answers question 1 directly instead of inferring it.

## Regenerating the disassembly

```sh
export PATH=$HOME/rtems/7/bin:$PATH
riscv-rtems7-objdump -d $SP/ble-scan.exe > $SP/full.dis          # blob + app
riscv-rtems7-objdump -D -b binary -m riscv:rv32 --adjust-vma=0x40000000 \
    <esp-qemu>/pc-bios/esp32c3-rom.bin > $SP/rom.dis             # the ROM
```

**Redirect to a file.** Piping `objdump` or `nm` through a shell pipeline here
returns addresses shifted by a constant — a proxy rewrites tool output. Reading
the file back with `/usr/bin/sed` is self-consistent with `readelf`. This cost
one subagent a detour and is worth knowing before the next one starts.
