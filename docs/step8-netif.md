# Step 8: the netif, and the one thing in it worth reviewing

This is README step 5 -- "a netif joining it to rtems-lwip" -- and it is the
piece between "the libraries initialise" and "packets move". Nothing above it
can be tested without it: not `network.WLAN`, not ESPHome's `wifi:`, not an IP
address.

Two files:

| | |
|---|---|
| `include/rtems-esp/netif.h` | the driver interface, and the netif's API |
| `src/rtems_esp_netif.c` | the netif, and the driver over the blobs |

`include/esp_netif.h` is unchanged in substance and that is the finding, not an
omission -- see "Why `esp_netif.h` stayed two typedefs wide" below.

Most of this document is about RX buffer ownership, because that is the part
most likely to be wrong and the hardest to see later. The rest is a list of
what is not verified, which on this step is nearly everything.

## The buffer ownership contract

`esp_private/wifi.h` declares the receive callback as

```c
typedef esp_err_t (*wifi_rxcb_t)(void *buffer, uint16_t len, void *eb);
```

and the `eb` has to go back through `esp_wifi_internal_free_rx_buffer()`. The
pool it comes from is sized by `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` (10) and
`CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` (32) in
`include/rtems-esp/sdkconfig.h`.

So a handle that is not returned is not an error anywhere. It is ten or
thirty-two frames of entirely normal operation, and then a radio that has gone
quiet, with no message and nothing in any status register to say why. That
asymmetry -- cheap to get right now, expensive to find later -- is the reason
the design is arranged around it rather than around throughput.

### The contract as implemented

**The release lives in the netif, not in the driver, and there is exactly one
call site.** `rtems_esp_netif_input()` is three steps in this order:

1. copy the frame into a pbuf, which may fail;
2. release the handle, unconditionally;
3. hand the pbuf to lwIP, which may also fail, by which time the handle is
   already back.

The validation at the top is an `if` rather than an early `return` for that
reason and no other: a `return` above step 2 is the bug this shape exists to
make impossible. A driver underneath cannot leak the handle by forgetting to
free it, because it never frees it.

**The frame is copied, not referenced.** lwIP can wrap a foreign buffer
instead: `pbuf_alloced_custom()` with `PBUF_REF` and a custom free that returns
the handle. That is what ESP-IDF's `wlanif` does with `CONFIG_LWIP_L2_TO_L3_COPY`
off, and it is what ESP32 Open MAC gets by going through `esp_netif` -- their
`openmac_netif_receive()` passes the buffer as both frame and handle and the
free happens later, through the driver's `driver_free_rx_buffer`.

The copy costs one `memcpy` of at most 1518 bytes. What it buys is that the
handle's lifetime is the body of one function. With `PBUF_REF` the lifetime
becomes "until the tcpip thread frees a pbuf", the release moves into a
callback that must also fire on lwIP's internal error paths, and the leak is
invisible again. The three-line invariant above is the whole return on that
memcpy.

**It is observable.** `eb_taken` and `eb_released` in
`rtems_esp_netif_stats` must be equal whenever no `rtems_esp_netif_input()`
call is in progress. A test can assert that.

It can also pass without testing anything, which is why the counter comment
says so: in emulation no frame arrives, both counters stay zero, and the
assertion holds vacuously. Read `rx_frames` first. A zero there means the
check proved nothing.

### The transmit side has no such contract, and that is checked rather than assumed

`esp_wifi_internal_tx()`'s own header: "This API makes a copy of the input
buffer and then forwards the buffer copy to WiFi driver." So the caller keeps
the buffer and lwIP's pbuf can be freed as usual the moment the call returns.
No completion callback, no TX-done queue, no reference counting.

The alternative primitive, `esp_wifi_internal_tx_by_ref()`, hands the pbuf
itself down with a reference count and a pair of callbacks registered through
`esp_wifi_internal_reg_netstack_buf_cb()`. It saves the copy and buys a second
buffer-ownership contract with a second way to leak. One is enough to get
right, and the RX one cannot be avoided.

### Where the copy actually happens on transmit

`LWIP_NETIF_TX_SINGLE_PBUF` defaults to 0 and rtems-lwip does not set it, so
lwIP may hand over a chain -- and a TCP segment normally is one, a header pbuf
followed by a data pbuf, so the chained case is the common one and not the
exception. A single pbuf is transmitted straight from `p->payload`; a chain is
linearised into one staging buffer.

That buffer is guarded by a mutex, and the reason is worth stating because the
Zynq driver's answer is different. `linkoutput` is not reached from one thread
only: with `LWIP_TCPIP_CORE_LOCKING` -- which rtems-lwip leaves at lwIP's own
default of 1, and which a `config.ini` entry can turn off -- an application
thread in sockets takes the core lock and runs the output path itself. Which
threads arrive here is therefore a configuration property of the stack, not
something a driver is entitled to assume. Two frames interleaved in one buffer
go out as two corrupt frames and nothing reports an error.

## Which lwIP entry point, and why the answer follows from the context

`netif_add()` is passed `tcpip_input`, and the receive path calls
`netif->input()`. Both halves of that are deliberate.

**The callback runs on the WiFi task, not in an ISR.** It is invoked by the
WiFi libraries from the task the OS adapter creates through
`wifi_osi_funcs_t`. So blocking is permitted, which is what makes
`pbuf_alloc()` legitimate here at all -- an ISR context would have ruled it
out and forced a queue and a second task.

**Spending time there is still not permitted.** With
`LWIP_TCPIP_CORE_LOCKING_INPUT` at lwIP's default of 0, `tcpip_input()`
allocates a message, posts it to the tcpip thread and returns, so
`ethernet_input`, `ip4_input`, `tcp_input` and the application's socket wake-up
all run on the tcpip thread. `netif_input()` -- lwIP's other entry point, for
`NO_SYS` builds -- would run all of that on the WiFi task instead, which means
the radio is unserviced for the duration and means lwIP re-enters `esp_wifi`
from inside the WiFi task when a response goes out. That is the wrong one here
for both reasons.

This is the same arrangement rtems-lwip's Zynq Cadence GEM driver uses: it
passes `tcpip_input` to `netif_add()` and calls `netif->input()` from its
`xemacif_input_thread`. The WiFi task stands where that thread does.

`tcpip_input` returning non-`ERR_OK` means it did not take the pbuf -- no
message from `MEMP_TCPIP_MSG_INPKT`, or a full `TCPIP_MBOX_SIZE` mailbox -- so
the caller must free it. That line is in the receive path and counted as
`rx_dropped_stack_busy`.

## Link state runs on the tcpip thread, which the Xilinx code does not do

`netif_set_link_up()` walks the netif's client data and, with `LWIP_DHCP` on,
calls into `dhcp_network_changed_link_up()`. It is core work. The WiFi event
handlers run on the event task from `src/rtems_esp_event.c`, which is neither
the tcpip thread nor a holder of the core lock, so the four link-state
transitions go through `tcpip_callback()` -- which runs the function on the
tcpip thread whatever `LWIP_TCPIP_CORE_LOCKING` happens to be set to, and so
does not depend on a configuration this file cannot see.

The vendored Xilinx code in rtems-lwip calls `netif_set_link_up()` straight
from its own `link_detect_thread` (`netif/xadapter.c`, lines 426 and 444).
That is unsynchronised access to the stack, and it is not a precedent worth
matching just because it is the local idiom.

The netif also starts **link-down**, where the GEM driver sets
`NETIF_FLAG_LINK_UP` in its init. That difference is real information rather
than taste: an Ethernet MAC has nothing better to say until its PHY thread
runs, whereas association is an event the WiFi libraries report. Starting down
and rising on `WIFI_EVENT_STA_CONNECTED` is what makes DHCP begin when there
is an AP to answer it.

## The substitution seam

rtems-esphome#99 records the reason: the blob route cannot be proposed to
upstream RTEMS -- eight binary archives and a 120-entry table shimming someone
else's OS abstraction -- and ESP32 Open MAC is the only route that could. It is
also not a migration anyone should attempt this year, because their driver is
ESP32/Xtensa rather than C3, it still needs the blobs for PHY bring-up, and
WPA2 is unchecked on their own feature list. So the goal is not to migrate. It
is not to foreclose.

One indirection does that. `rtems_esp_netif_driver` is five function pointers:

| | |
|---|---|
| `attach` | install the receive path; the driver then calls `rtems_esp_netif_input()` |
| `detach` | remove it again; may be NULL |
| `transmit` | send one contiguous Ethernet frame, borrowed for the call |
| `release_rx` | give back one RX handle; may be NULL if the driver passes none |
| `get_mac` | the station's MAC address |

Two properties of that list matter more than its contents.

**Nothing in it mentions ESP-IDF.** The `int` returns are documented as
"zero for success", not `esp_err_t`, specifically so the header does not pull
in `esp_err.h` and with it the ESP-IDF include path. An Open MAC driver, or a
future RTEMS-native one, implements five functions against lwIP and nothing
from Espressif.

**It is measured, not asserted.** After compiling, `tools/netif-build.sh`
runs `nm -u` over the object and prints what it asks of `esp_wifi`:

```
U esp_wifi_get_mac
U esp_wifi_internal_free_rx_buffer
U esp_wifi_internal_reg_rxcb
U esp_wifi_internal_tx
```

Four names, all of them inside the blob driver at the bottom of the file below
the comment band. A netif that grew a fifth dependency would show up on that
line, which is the point of printing it on every build rather than checking it
once.

`release_rx` being nullable is the concession to Open MAC's shape specifically:
their MAC stack recycles its own frame buffers and does not hand out a handle
at all, so such a driver passes NULL as `eb` and leaves `release_rx` NULL.

### What was deliberately not abstracted

There is **one** netif, a station, in a static. That is a property of the seam
rather than a shortcut: `esp_wifi_internal_reg_rxcb()` takes a bare function
pointer with no context argument -- "Currently we support only one RX callback
for each interface" -- so the callback has to find its netif through a static
no matter what else is built. Given that, a second interface is a second static
and a second trampoline, not a data structure. AP mode would need those two
things, plus `WIFI_IF_AP` in the blob driver's four calls, plus a DHCP server,
which is a different piece of work.

## Why `esp_netif.h` stayed two typedefs wide

The port needs an interface, not `esp_netif`'s API. Filling `esp_netif` in
would mean implementing `esp_netif_new`, `_attach`, `_set_driver_config`, the
`esp_netif_action_*` family and an `IP_EVENT` loop -- and then implementing an
lwIP netif underneath all of it anyway, because that is what `esp_netif` is a
wrapper for. ESP32 Open MAC does go that way, and correctly: they are inside
ESP-IDF and `esp_netif` is already built for them. Here it would be a second
abstraction with one user.

So the file keeps the two opaque typedefs that let `esp_wifi_default.h` parse,
and gains a comment recording that the netif landed elsewhere.

A correction while in the area. `docs/step3.md`, the README and the issue
thread all say `esp-hal-3rdparty` "ships 34 components". A full checkout of
`sync/master.c` has **100** -- `ls components | wc -l`. The claim the number
was supporting is unaffected, and is the one that matters: `esp_netif` is not
one of them, whichever count is quoted.

One thing did change in it, and it is worth knowing because it cost time. Its
include guard was `RTEMS_ESP_NETIF_H`, which is also the natural guard for
`<rtems-esp/netif.h>`. With both present, a file including the real netif
header first got `esp_netif.h` silently skipped, and the failure arrived as
ESP-IDF's own `esp_wifi_default.h` not knowing what `esp_netif_t` is -- loud,
but a long way from the cause. The compatibility shim is now
`RTEMS_ESP_NETIF_COMPAT_H`.

## lwIP is not in the BSP prefix, and rtems-lwip has no RISC-V BSP

This is a blocker for the step after this one and it is not worked around
anywhere.

**The prefix has no lwIP.** `iram-prefix/riscv-rtems7/esp32c3db/lib` holds

```
libftpd.a  libftpfs.a  libjffs2.a  librtemsbsp.a  librtemscpu.a
librtemscxx.a  librtemsdefaultconfig.a  librtemstest.a  libtftpfs.a  libz.a
```

There is no `liblwip.a`, and `lib/include` has no `lwip/` directory --
`find` over the whole prefix for anything matching `lwip*` returns nothing.
That is not a misconfiguration: rtems-lwip is a separate package with its own
waf build and it was never built for this BSP.

The sharper version of that statement, since it is easy to read the absence as
"nobody has run the install step yet": searching every prefix on this machine
for `liblwip.a` finds it in exactly one place, `arm-rtems7/xilinx_zynq_a9_qemu`.
The only lwIP that exists here is the Zynq's, which is the one the MicroPython
networking work ran against.

**It cannot currently be built for this BSP either.** `defs/bsps/` in
rtems-lwip has `aarch64`, `arm` and `sparc`, and no `riscv` at all, so
`./waf configure --rtems-bsps=riscv/esp32c3db` has no `esp32c3db.json` to read.
Three files have to exist before lwIP can be built for the C3:

| file | what it is |
|---|---|
| `defs/bsps/riscv/esp32c3db.json` | the driver sources and include paths for the BSP |
| `rtemslwip/esp32c3/lwipbspopts.h` | the BSP's lwIP options |
| `rtemslwip/esp32c3/netstart.c` | `start_networking()`, the equivalent of `rtemslwip/zynq/netstart.c` |

**And rtems-lwip's defaults do not fit the part.** `rtemslwip/include/lwipopts.h`
sets `MEM_SIZE` to 2 MiB and `PBUF_POOL_SIZE` to 512 at `PBUF_POOL_BUFSIZE`
1600 -- 800 KiB of static pool plus a 2 MiB heap, on a part with 400 KiB of
SRAM, of which the WiFi libraries already claim 54 KiB. It is not a tight fit
to be tuned; it is out by an order of magnitude and the BSP file has to
override it. `tools/netif-build.sh` writes the `lwipbspopts.h` it compiles
against, and those numbers are the proposal:

```c
#define MEM_LIBC_MALLOC   1
#define MEMP_MEM_MALLOC   1
#define MEM_SIZE          ( 32 * 1024 )
#define PBUF_POOL_SIZE    16
#define PBUF_POOL_BUFSIZE 1600
```

1600 is rtems-lwip's own bufsize and covers a 1518-byte frame in a single
pbuf, which keeps the receive path's `pbuf_take()` one `memcpy`. Sixteen of
them is 25 KiB.

Because the prefix has no lwIP, `tools/netif-build.sh` takes the headers from
an rtems-lwip **source** checkout, through the same four include paths
`defs/common/lwip.json` gives its own build, in the same order. They are the
real port's real headers, so what compiles here compiles there. What it cannot
do is link.

## What was actually run

Exactly four things, and it is worth being precise about which:

**1. It compiles.** `tools/netif-build.sh`, with `-Wall -Wextra -Werror`,
produces a 3936-byte-text object. Not vacuous: `nm` over it shows 29 defined
symbols, including `rtems_esp_netif_add`, `rtems_esp_netif_input` and
the blob driver table, and the undefined list contains `netif_add`,
`tcpip_input`, `tcpip_callback`, `pbuf_alloc`, `pbuf_take` and
`pbuf_copy_partial`, so the lwIP calls are compiled in rather than configured
out.

**2. The blob half of the seam links.** A real `gcc` link -- the BSP's
`linkcmds` through `-qrtems -B`, the seven ROM scripts, all eight blobs, the
port's nine other objects, `-lm` -- resolves all four `esp_wifi_*` symbols out
of the archives. Adding the netif object to that link adds **thirteen**
unresolved symbols and **all thirteen are lwIP**:

```
etharp_output  ethip6_output  netif_add  netif_set_down  netif_set_link_down
netif_set_link_up  netif_set_up  pbuf_alloc  pbuf_copy_partial  pbuf_free
pbuf_take  tcpip_callback  tcpip_input
```

**3. The control for that.** The same link without the netif object, which is
what says the thirteen are the netif's and the six pre-existing
`rtems_esp_wifi_{iram,dram}_*` are the section script's rather than mine. It
was worth running: the first attempt at the probe reported *zero* unresolved
symbols, which read as a clean link and was actually `zsh` not word-splitting
the object list, so `ld` was handed one long filename and failed on its first
argument. That is the fourth time this project has been caught by a check that
passed because the thing under test never ran, and the reason
`tools/netif-build.sh` checks its inputs before using them.

**4. The blob symbols exist where the header says.** `riscv-rtems7-nm`
over the C3 archives:

| symbol | archive | object |
|---|---|---|
| `esp_wifi_internal_tx` | `libnet80211.a` | `ieee80211_output.o` |
| `esp_wifi_internal_reg_rxcb` | `libnet80211.a` | `ieee80211_api.o` |
| `esp_wifi_internal_free_rx_buffer` | `libpp.a` | `if_hwctrl.o` |
| `esp_wifi_get_mac` | `libnet80211.a` | `ieee80211_api.o` |

## What is not verified

Everything about whether a packet moves. Stated item by item, because "it
compiles" and "it works" are separated by all of this.

**No frame can move in emulation, at all.** QEMU's `esp32c3` model has no WiFi
MAC. There is nothing for the PHY and MAC registers to be backed by, so there
is no configuration of this software in which a frame arrives.

**The image does not even reach the libraries.** As recorded in the previous
step, the test image stalls in PHY calibration -- `txdc_cal_v70+0xcc` -- before
`esp_wifi_init_internal()` returns, and `PHY_RF_CAL_NONE` is not a way around
it. So not one line of this file has executed. Not the `netif_add`, not the
event registration, not the receive callback.

**It has never been linked into a complete image**, because there is no
`liblwip.a` for this BSP. The thirteen lwIP symbols above are the whole of
that gap.

Specifically unverified, and in rough order of how likely each is to be wrong:

* **That the receive callback returns every `eb`.** The invariant is arranged
  to be readable and the counters are there to assert it, but it has never run
  once. This is the main risk and it stays the main risk.
* **That `WIFI_EVENT_STA_START` is a legal moment to call
  `esp_wifi_internal_reg_rxcb()`.** The header says the callback is per
  interface and says nothing about ordering. `STA_START` is the first moment
  at which the interface certainly exists, which is a reason and not a
  measurement.
* **That passing NULL to `esp_wifi_internal_reg_rxcb()` unregisters.** That is
  ESP-IDF's idiom, not a documented contract. If it is wrong, the consequence
  is a callback still installed after stop, which
  `rtems_esp_netif_input()` survives -- it drops the frame and still returns
  the handle.
* **That the callback context is what this assumes.** It is documented here as
  the WiFi task, on the reasoning that the libraries reach the OS only through
  `wifi_osi_funcs_t`. If it were ever an ISR, `pbuf_alloc()` would be illegal
  there and the shape would have to change to a queue and a task.
* **The MTU, the flags and the hwaddr**, none of which have been seen by an
  ARP exchange.
* **`esp_wifi_internal_set_sta_ip()`.** ESP-IDF calls it when DHCP completes
  and this does not call it at all. Wiring it needs
  `LWIP_NETIF_STATUS_CALLBACK`, which is 0 in rtems-lwip's defaults, so it is
  a known gap rather than an oversight. What it affects is power-save
  behaviour and DHCP-frame filtering, so an always-on station may not notice.
* **Anything about AP mode**, which is not implemented.

What would settle it is `tools/zynq-lwip-run.sh`'s claim shape -- a TCP
connection accepted from outside the guest with bytes moving -- on hardware.
That is the only test of this file worth reporting, and it needs an ESP32-C3
on a desk.
