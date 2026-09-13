# Step 9: one image, and what an image makes checkable

Three pieces have each worked alone and no two of them had ever been in the
same binary. `liblwip.a` builds for `riscv/esp32c3db` and reaches `socket()`,
with no interface, because `start_networking()` leaves that to a weak
`esp32c3_netif_add()`. `src/rtems_esp_netif.c` implements an lwIP netif over
`esp_wifi_internal_tx()` and `esp_wifi_internal_reg_rxcb()` and had never
executed a line. `examples/wifi-init` starts the radio with no stack above it.

`examples/wifi-net` is the join. It is `wifi-init` plus `-llwip`, the netif
object, and the checks that only exist once all three are present.

Nothing below is a claim that a packet moved. The last section says what is
still unverified, and it is still most of what matters.

## What the image contains

| | |
|---|---|
| `examples/wifi-net/init.c` | the application, and the checks |
| `src/rtems_esp_netif.c` | the netif and the blob driver, compiled here for the first time into a linked image |
| `liblwip.a` | rtems-lwip built for `riscv/esp32c3db`, from branch `esp32c3-port` |
| the eight blobs, the supplicant, mbedTLS | exactly as `wifi-init` links them |

`examples/wifi-net/build-and-run.sh` is `wifi-init`'s script with three
additions and no subtractions. The generated linker script, the four ROM
scripts and the two `-u` flags for the supplicant are carried over unchanged,
along with the comments saying why each is the way it is; every one of them
records a failure that was silent.

## The symbol that had to win

`rtemslwip/esp32c3/netstart.c` declares

```c
int __attribute__((weak)) esp32c3_netif_add(
  struct netif *, ip_addr_t *, ip_addr_t *, ip_addr_t *, unsigned char *
);
```

and its own definition prints "the stack is up but there is no interface" and
returns 1. The netif has to define that symbol strongly.

A weak definition satisfies a reference, so `ld` never searches an archive for
the strong one — which is how the supplicant stub won a link for an hour in
step 6 and produced an image that could not do WPA2. The same trap is here in
mirror image: the weak definition is *inside* `liblwip.a`, arriving
unavoidably with `start_networking()` in the same object.

The answer is that `rtems_esp_netif.o` is named on the command line rather than
put in an archive. An object named on the command line is always loaded, so the
strong definition is in the symbol table before `liblwip.a` is searched. That
is the theory; the build script checks it:

```sh
kind=$(riscv-rtems7-nm wifi-net.exe | awk '$3 == "esp32c3_netif_add" { print $2 }')
[ "$kind" = "T" ] || exit 2
```

`W` there would mean the do-nothing default is in the image, which links
cleanly, boots cleanly and gives the application no interface.

## What the netif needed

One change, and it is about whose `struct netif` gets added.

`rtems_esp_netif_add()` used its own file-static `struct netif`. That is fine
for a caller that then asks `rtems_esp_netif_get()`, and wrong for the
rtems-lwip convention, where `start_networking()` takes the application's netif
and hands it down. An application following that convention — `tests/zynq-lwip`
checks `netif_is_up(&net_interface)` on the one it passed — would have been
asking about a zeroed bystander while the real interface lived elsewhere.

So the static is now a static *pointer*, to storage the caller may supply, with
the file's own used when the caller passes NULL. The reason the netif is
singular and reached through a static is untouched by this:
`esp_wifi_internal_reg_rxcb()` takes a bare function pointer with no context
argument, so the receive callback has to find its netif through a static
whatever else is true.

`esp32c3_netif_add()` itself is new, at the bottom of the file with the blob
driver, because it names `rtems_esp_netif_blob_driver`. Three things about its
arguments are worth stating, because each is a place where the rtems-lwip
convention and a radio disagree:

* **The addresses are `ip_addr_t`, the dual-stack union**, because `LWIP_IPV6`
  is 1 in this BSP's `lwipopts.h`. `netif_add()` wants the `ip4_addr_t` inside
  it, and reading that out of a union tagged IPv6 would configure nonsense
  silently, so the tag is checked rather than assumed.
* **`mac_address` is accepted and not used.** On the Zynq it is an input: the
  driver programs the GEM with whatever the application chose. A station's MAC
  is not the application's to choose — it comes from eFuse, and the libraries
  derive the interface addresses from it, so a netif claiming a different one
  would ARP for addresses the radio never accepts. The run below checks that
  the passed address was in fact ignored.
* **`netif_set_default()` is called and `netif_set_up()` is not.** This is the
  only interface the part has, so it is the default route whether or not it is
  up; without that every `connect()` off the loopback network fails with
  `ERR_RTE`. Coming up waits for `WIFI_EVENT_STA_START`, which is the moment
  the receive path can be installed.

The seam held. `nm -u` over the compiled netif object still shows four
`esp_wifi_*` symbols and no more:

```
esp_wifi_get_mac  esp_wifi_internal_free_rx_buffer
esp_wifi_internal_reg_rxcb  esp_wifi_internal_tx
```

## What the run prints

```
*** ESP32-C3 WIFI + LWIP TEST ***
esp_event_handler_register                           ok
rtems-esp-wifi: copied 44772 bytes of IRAM and 496 of DRAM
rtems-esp-wifi: register_chipv7_phy( PHY_RF_CAL_FULL )...
rtems-esp-wifi: no saved calibration, so the PHY calibrated fully
rtems-esp-wifi: PHY registered, esp_wifi_init_internal()...
wifi.pp: pp rom version: 9387209
wifi.net80211: net80211 rom version: 9387209
rtems-esp-wifi: libraries initialised, esp_supplicant_init()...
esp_wifi_init                                        ok
esp_wifi_set_mode(STA)                               ok
calling start_networking...
start_networking                                     ok
the netif is registered and is the default route     ok
and it is the one the port reports                   ok
named wl0                                            ok
with an Ethernet MTU                                 ok
and the address it was given                         ok
esp_wifi_get_mac                                     ok
       esp_wifi_get_mac  00:00:00:00:00:00
       netif->hwaddr     00:00:00:00:00:00
the netif reports the MAC the libraries do           ok
which is not the one the application passed          ok
       both are all-zero, which is what QEMU's eFuse reads as.
       So read_mac ran and the netif took its answer; whether that
       answer is right needs a part with a real eFuse.
the interface starts link-down, as a radio should    ok
calling esp_wifi_start...
esp_wifi_start                                       ok
       event: WIFI_EVENT id 43
       event: WIFI_EVENT id 2
the receive path was accepted and the interface came up ok
       linkoutput returned -12 for a single pbuf, -12 for a chain
two transmits reached the driver and returned        ok
the chained one was linearised                       ok
neither was rejected for length                      ok
       esp_wifi_internal_tx answered 12294 (0x3006) directly
socket()                                             ok

netif counters:
rx_frames             0
...
tx_linearised         1
tx_dropped_too_long   0
tx_dropped_driver     2
link_up               0
link_down             0

0 failure(s)
CI-MARKER wifi net ok
```

Four of those lines carry more than they look.

**`event: WIFI_EVENT id 2`** is `WIFI_EVENT_STA_START`; **id 43** is
`WIFI_EVENT_HOME_CHANNEL_CHANGE`, counted from the enum in
`esp_wifi_types_generic.h` rather than guessed. This is the first time the
port's event path has been seen delivering an event to a handler at all —
`wifi-init` prints none, because its `Init` task runs at priority 1, the event
task at 100, and `Init` calls `exit()` before ever blocking.

**"the interface came up"** is the one check worth reading twice. `netif_set_up()`
is reached only from `rtems_esp_netif_start()`, which only the
`WIFI_EVENT_STA_START` handler calls, and only after the driver's `attach()` —
`esp_wifi_internal_reg_rxcb()` — has returned success. So the boolean says
four things at once: the event was dispatched on its own task, the netif's
handler was registered in time to hear it, the libraries accepted the receive
callback, and `tcpip_callback()` got the work onto the tcpip thread. It cannot
pass without all four.

It says nothing about a frame arriving. `esp_wifi_internal_reg_rxcb()`
returning success means the callback was *accepted*, not that it will ever be
called.

**`linkoutput returned -12`**, `ERR_IF`, twice, with `tx_dropped_driver` at 2
and `tx_linearised` at 1. Both pbuf shapes went through
`rtems_esp_netif_linkoutput()` into `esp_wifi_internal_tx()` and came back:
`tx_linearised` can only be incremented after `pbuf_copy_partial()` has filled
the staging buffer, and `tx_dropped_driver` on that path only after the
driver's `transmit` returned. The refusal is expected — the station has not
associated with anything.

**`esp_wifi_internal_tx answered 12294 (0x3006)`** is why the refusal is
informative rather than merely a non-zero. `ESP_ERR_WIFI_BASE` is `0x3000` and
`+6` is `ESP_ERR_WIFI_STATE`, "WiFi internal state error". That code comes from
inside `libnet80211.a`; a zero from a stub, or a fault, would look quite
different.

### The green result was checked for vacuity

"Could this have passed without testing anything" is the failure mode this
project keeps hitting, so the interesting check was made to fail on purpose.
Removing the `rtems_task_wake_after()` — so that the priority-1 `Init` task
never yields and the priority-100 event task never runs — gives:

```
the receive path was accepted and the interface came up FAIL
1 failure(s)
```

with no `event:` lines at all, and the transmit checks still passing. So the
check depends on the event actually being dispatched, and the transmit checks
do not depend on the interface being up, which makes them independent evidence
rather than a second reading of the same fact.

## The memory, measured rather than estimated

This is the number rtems-esphome#108 asks for. lwIP's `PBUF_POOL_SIZE` and
`MEM_SIZE` were chosen against "218 KiB free", and that measurement came from
an image with no lwIP in it. `CONFIGURE_UNIFIED_WORK_AREAS` makes `.work` both
the RTEMS workspace and the C heap, and the linker script sizes it to whatever
is left of the 320 KiB RAM region, so its extent *is* the free memory.

| | `wifi-init` | `wifi-net` | difference |
|---|---:|---:|---:|
| `text` | 886,639 | 1,096,785 | +210,146 |
| `data` | 7,588 | 7,708 | +120 |
| `.bss` | 29,132 | 114,124 | +84,992 |
| `.work` | 220,560 | 135,184 | **-85,376** |

So lwIP costs **83.4 KiB of RAM** and leaves **132.0 KiB** free, against
215.4 KiB before. The text growth is 205 KiB and is in flash, of which there
is 4 MiB; it is not a constraint.

Where the 83.4 KiB goes, from `nm -S`:

| symbol | size |
|---|---:|
| `memp_memory_PBUF_POOL_base` | 38,784 (37.9 KiB) |
| `ram_heap` | 32,780 (32.0 KiB) |
| the other `memp_memory_*` pools, socket and netif tables | ~13 KiB |

The first two are the 24 × 1600 pbuf pool and `MEM_SIZE`, and they are within a
few hundred bytes of the 70 KiB the sizing comment in `lwipbspopts.h` claims
for them. What the estimate missed is the third row: lwIP's other pools and
tables are about 13 KiB and were not counted. The two images also differ by the
example's own code, which is a few hundred bytes of that total, so read 83.4 KiB
as "lwIP and a little" rather than as lwIP exactly.

Two things the table does not include. `.work` is measured before anything runs,
and the tcpip thread and the interrupt server take their stacks out of it at
`start_networking()`. And this application is small: an ESPHome node's tasks and
buffers come out of the same 132 KiB.

The headline is that it fits with room, and that the pool is now the thing to
revisit first if it does not: `PBUF_POOL_SIZE` 24 is 37.9 KiB, it is the second
queue behind the WiFi libraries' own RX buffers, and nothing has yet put a
frame through either of them.

## What is still not verified

Unchanged from step 8 in substance, and now sharper because the code exists in
a running image rather than only in an object file.

**No frame moved, and none can.** QEMU's `esp32c3` model has no WiFi MAC: it
raises no interrupt and injects nothing. `rtems_esp_netif_input()` never
executed, so the RX buffer-ownership contract — the main risk, and still the
main risk — has never run. `eb_taken == eb_released` in the counters above
holds vacuously, which is why `rx_frames` is printed first and why the example
says so in words.

**Nothing associated**, so:

* `WIFI_EVENT_STA_CONNECTED` and `WIFI_EVENT_STA_DISCONNECTED` never arrived.
  `link_up` and `link_down` are 0, and the `tcpip_callback()` path for link
  state has not run. Only the `STA_START` case of the netif's event handler has
  executed.
* `rtems_esp_netif_stop()` and the driver's `detach()` have not run, so whether
  passing NULL to `esp_wifi_internal_reg_rxcb()` unregisters is still ESP-IDF
  idiom rather than a measurement.
* DHCP has not run. The address here is static on purpose.
* No frame left the chip. `esp_wifi_internal_tx()` was reached and refused.

**The MAC is all zeros.** `esp_wifi_get_mac()` returned `ESP_OK`, so the
adapter's `read_mac` executed and the netif took its answer, and the check that
the netif ignored the application's address is real. But two all-zero addresses
agree for free: whether the value is *right* needs a part with a real eFuse.

**The callback's context is still assumed, not observed.** It is documented as
the WiFi task on the reasoning that the libraries reach the OS only through
`wifi_osi_funcs_t`. Since it has never been invoked, that remains reasoning.

What would settle the rest is `tools/zynq-lwip-run.sh`'s claim shape — a TCP
connection accepted from outside the guest with bytes moving in both
directions — on an ESP32-C3 on a desk, or a QEMU MAC model (#102).
