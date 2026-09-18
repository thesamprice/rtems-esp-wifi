# Building the supplicant with its own logging

`wpa_printf()` is gated on `DEBUG_PRINT`, and without it every line the
supplicant would say about a four-way handshake compiles to `do {} while (0)`.
Turning it on is what showed the handshake completing, which had been guessed
at both ways for a long time.

It is not a one-line change, because `libwpa.a` has no build system (#100).
This is the recipe that works, written down so the next person does not
rediscover it.

## The member list

The archive is the 131 prebuilt objects in the scratchpad's `wpa/all/`, not the
48 in `wpa/tus.txt`. Nothing recorded that, and every attempt to narrow it --
excluding what looked like the wrong port, closing over undefined symbols,
rebuilding from `wpa/wpadef.txt` -- produced an archive that did not link.
`wpa/srclist.txt` maps those object names back to sources.

## The flags that are not obvious

Three, beyond `-DDEBUG_PRINT`:

* `-DIEEE8021X_EAPOL`. Without it `pmksa_cache.h` takes its `#else` branch,
  which defines `pmksa_cache_remove()` with a *body* in a header, and every
  translation unit that includes it emits a copy: "multiple definition of
  `pmksa_cache_remove'". It is also the correct setting on its own terms --
  EAPOL is what the four-way handshake runs over.
* `CONFIG_LOG_MAXIMUM_LEVEL` must be 5. It is the compile-time ceiling, so at
  the ESP-IDF default of 3 every `wpa_printf(MSG_DEBUG, ...)` is removed by the
  preprocessor -- which is most of the handshake. Raising the runtime filter
  alone changes nothing and looks exactly like a supplicant that never runs.
* `CONFIG_LOG_VERSION` must be 2. Version 1's `ESP_LOG_LEVEL` expands into a
  chain wanting `CONFIG_LOG_TIMESTAMP_SOURCE_*` and `LOG_FORMAT`; undefined,
  `ESP_LOG_VERSION` reads as 0 and takes that branch.

## Three objects must come from the prebuilt set

Recompiling all 131 leaves five files failing (they want `CONFIG_WPS` /
`CONFIG_EAP`) and three symbols undefined. Take these from `wpa/all/`:

* `src_crypto_sha1-internal.o` -- defines `sha1_vector`; the recompiled one is
  empty because it is guarded by `CONFIG_CRYPTO_INTERNAL`
* `esp_supplicant_src_esp_wps.o` -- defines `wps_get_wps_sm_cb`, which
  `esp_wpa_main.c` calls unconditionally

and extract `platform_util.o` from `wpa/libmbedtls_rtems.a` for
`mbedtls_platform_zeroize`, which `port/os_xtensa.c` needs and `libmbedtls.a`
does not define. Adding the whole archive instead gives duplicate definitions
against `libmbedtls.a` -- the two are different builds of the same library.

## Running it

`RTEMS_ESP_LOG_LEVEL=5` on the glue, and expect the timing cost: printk drains
the console synchronously, and at this volume the handshake can miss its timing
and disconnect with `WIFI_REASON_ASSOC_EXPIRE`. It is a diagnostic build, not a
configuration.
