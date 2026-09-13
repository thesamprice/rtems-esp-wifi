# Step 6: making wpa_supplicant's configuration coherent

Step 5 ended with the supplicant fetched and compiling, and with
`esp_supplicant_init()` and `g_wifi_default_wpa_crypto_funcs` coming from
`libwpa.a` instead of from the weak stubs. The image still did not link: 43
symbols were undefined, among them `ccmp_encrypt`, `gWpaSm`, `sha1_vector`,
`wpa_cipher_key_len` and `eloop_init`.

None of them were missing sources. `src_crypto_ccmp.o` was in the archive and
did not define `ccmp_encrypt`. This is what that turned out to be.

## The mistake: putting the supplicant's macros in sdkconfig.h

The supplicant is one body of source covering WPA2-PSK, WPA3-SAE, enterprise
EAP, WPS, DPP and Wi-Fi Aware, and it selects among them with `CONFIG_*`
macros that change function sets **and struct layouts**. The obvious place to
set them is `include/rtems-esp/sdkconfig.h`, since that is where every other
`CONFIG_*` in this port lives, and that is where they were.

It does not work, and the way it fails is quiet.

`sdkconfig.h` reaches a supplicant source only indirectly:
`src/utils/includes.h` includes `port/include/supplicant_opt.h`, which includes
`sdkconfig.h`. So a `#define` there lands *below* the first `#include` and
nowhere above it. Eleven of the 147 sources test a feature macro above their
first `#include`. `src/crypto/ccmp.c` shows why that is fatal:

```c
#ifdef CONFIG_IEEE80211W        /* line 9  */

#include "utils/includes.h"     /* line 11 */
```

The include that would define the macro sits inside the `#ifdef` that the macro
gates. The file cannot switch itself on. Compiled both ways with everything
else held constant:

| | size | defined symbols |
|---|---|---|
| `ccmp.o` without `-DCONFIG_IEEE80211W` | 860 bytes | 0 |
| `ccmp.o` with `-DCONFIG_IEEE80211W` | 5824 bytes | 8 |

The 860-byte object went into `libwpa.a` and satisfied nothing, while
`src/rsn_supp/wpa.h` — which includes `sdkconfig.h` at the top, above any test
— saw `CONFIG_IEEE80211W` set and laid out `struct wpa_sm` accordingly. Half
the library believed in protected management frames and half did not. Nothing
warned. The only symptom was a link error naming a function whose source file
was visibly present.

Upstream never has the problem because it never uses `sdkconfig.h` for these.
`components/wpa_supplicant/CMakeLists.txt` passes them with

```cmake
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    __ets__ ESP_SUPPLICANT IEEE8021X_EAPOL EAP_PEER_METHOD EAP_MSCHAPv2
    EAP_TTLS EAP_TLS EAP_PEAP USE_WPA2_TASK CONFIG_WPS ESPRESSIF_USE
    CONFIG_ECC CONFIG_IEEE80211W CONFIG_SHA256 CONFIG_NO_RADIUS)
```

— once, to every file, before any header is read. `tools/supplicant-build.sh`
now carries that list verbatim and `sdkconfig.h` carries the Kconfig-level
choices above it, as comments, because nothing in this port reads them.

That one change took the build from 118 sources to 127 and the undefined count
from 43 to 8.

## The eight that were left

Four distinct causes, each traced to a definition rather than guessed at.

**`sha1_vector`.** Defined in `src/crypto/sha1-internal.c` behind
`CONFIG_CRYPTO_INTERNAL`, and in `esp_supplicant/src/crypto/crypto_mbedtls.c`
otherwise. Upstream's rule is mechanical: `CMakeLists.txt` defines
`CONFIG_CRYPTO_INTERNAL` when neither `CONFIG_MBEDTLS_SHA1_C` nor
`CONFIG_MBEDTLS_HARDWARE_SHA` is set, and this port has neither — there is no
`MBEDTLS_SHA1_C` in `mbedtls_config.h` and nothing here drives the C3's SHA
peripheral. So `-DCONFIG_CRYPTO_INTERNAL`.

**`mbedtls_platform_zeroize`.** Not a supplicant question at all:
`libmbedtls.a` did not define it, and 27 of its own objects reference it. It
lives in `tf-psa-crypto/platform/platform_util.c`, one file in a directory
`tools/mbedtls-build.sh` was not globbing. An archive that cannot satisfy its
own members is quiet about it until something pulls one of the 27 in.

**`wps_get_wps_sm_cb`.** `esp_supplicant/src/esp_wps.c` did not compile, for
two unrelated reasons.

* `const char *wps_model_number = CONFIG_IDF_TARGET;` at file scope. ESP-IDF
  generates `CONFIG_IDF_TARGET "esp32c3"` alongside `CONFIG_IDF_TARGET_ESP32C3
  1` from one Kconfig choice; this port had only the second. Both are in
  `sdkconfig.h` now, two lines that have to agree.
* `static atomic_int s_wps_owner = ATOMIC_VAR_INIT(WPS_OWNER_NONE);`.
  `ATOMIC_VAR_INIT` was deprecated in C17 and removed in C23; this gcc defaults
  to `gnu23` (`__STDC_VERSION__` is 202311L) and its `stdatomic.h` hides the
  macro behind `__STDC_VERSION__ <= 201710L`. `-std=gnu17` in the build script.

`esp_wpa_main.c` calls `wps_get_wps_sm_cb()` unconditionally, WPS wanted or
not, so this is not optional.

**The five `eloop_*`.** A different kind of problem; see below.

## Why the crypto does not go through mbedtls

`CONFIG_ESP_WIFI_MBEDTLS_CRYPTO` defaults to `y` upstream, and this port has
mbedtls built. It is nevertheless off here, and not as a preference.

`esp_supplicant/src/crypto/crypto_mbedtls.c` is written against ESP-IDF's
mbedtls **component**, not against `espressif/mbedtls`. It includes
`mbedtls/esp_config.h`, `mbedtls/ecp.h` and `mbedtls/bignum.h`. In mbedtls 4.x
the last two exist only as `mbedtls/private/ecp.h` and
`mbedtls/private/bignum.h`; all three are supplied by
`components/mbedtls/port/include`, which is a component this port does not
build. Adding that directory to the include path is not the fix either:
`esp_config.h` is a 3224-line configuration that selects the ESP32 hardware
PSA drivers, and the compile then stops at

```
psa/crypto_driver_contexts_composites.h:56:10: fatal error:
    psa_crypto_driver_esp_hmac_opaque_contexts.h: No such file or directory
```

which is the C3's HMAC peripheral. Worse than the error is what would happen
if it were satisfied: `esp_config.h` redefines the `MBEDTLS_*` set that
`libmbedtls.a` was compiled with, so `crypto_mbedtls.o` would hold a different
`psa_hash_operation_t` than the library it calls. That is a silent ABI break,
which is the failure mode this whole step exists to remove.

So the port takes upstream's else-branch: the supplicant's own portable C
crypto — `aes-internal*.c`, `sha1-internal.c`, `sha256-internal.c`,
`crypto_internal*.c`. This is a combination upstream maintains, not a
hand-made one, and for a PSK station it is complete. The table the WiFi
libraries call through, read out of the linked image, is 13 words and no nulls:

```
size = 0x34, version = 1, then
hmac_sha256_vector  pbkdf2_sha1  aes_128_cbc_encrypt  aes_128_cbc_decrypt
omac1_aes_128  ccmp_decrypt  ccmp_encrypt  esp_aes_gmac  sha256_vector
aes_wrap  aes_unwrap
```

CCMP, PBKDF2 and the MIC — the three the issue named as faulting on a null
call — are `ccmp_encrypt`/`ccmp_decrypt`, `pbkdf2_sha1` and `omac1_aes_128`.
`aes_wrap`/`aes_unwrap` is the RFC 3394 group key in EAPOL-Key message 3.

The cost is speed, not capability. PBKDF2-SHA1 runs 4096 HMAC iterations in
software once per association. Nothing is lost to the C3's AES and SHA
peripherals, because this port does not drive them — the mbedtls built here is
software too.

`libmbedtls.a` therefore contributes nothing to the image under this
configuration: `nm` on `libwpa.a` shows exactly zero undefined `mbedtls_` or
`psa_` symbols. It is still built, because it is what SAE and enterprise EAP
will need and because building it is how we find out that it is not what
`crypto_mbedtls.c` wants.

## What is off, and what that costs

Nothing that was working was turned off to close the link. Every change added a
macro or an include path. For the record, the Kconfig-level choices and their
price:

| off | cost |
|---|---|
| `ESP_WIFI_ENABLE_WPA3_SAE`, `ESP_WIFI_ENABLE_WPA3_OWE_STA` | a WPA3-only AP will not accept this station; a WPA3-transition AP will, over WPA2-PSK |
| `ESP_WIFI_ENTERPRISE_SUPPORT` | no 802.1X network; also why `mbedtls_config.h` builds no TLS and no X.509 |
| `ESP_WIFI_MBEDTLS_CRYPTO` | software AES/SHA instead of mbedtls; see above |
| `ESP_WIFI_SOFTAP_SUPPORT` | station only |
| `ESP_WIFI_11KV_SUPPORT`, `RRM`, `WNM`, `MBO` | roaming decided on signal strength alone, with no neighbour report or BSS transition hint |
| `ESP_WIFI_DPP_SUPPORT`, `NAN_USD`, `PASN`, `WPS_SOFTAP_REGISTRAR` | off upstream too |

`CONFIG_IEEE80211W` is **on**. Upstream defines it unconditionally, and it
matters: WPA3-transition access points require protected management frames and
a growing number of WPA2-only ones are configured to, so a station without it
fails to associate with networks that look ordinary.

## What still blocks a zero-symbol link

`port/eloop.c`. It is the supplicant's timer wheel — `eloop_init`,
`eloop_register_timeout`, `eloop_cancel_timeout`, `eloop_destroy`,
`eloop_is_timeout_registered`, `eloop_register_timeout_blocking` — and the
4-way handshake's retransmission timers run on it, so it is not optional.

The `rom/ets_sys.h` and `esp32c3/rom/ets_sys.h` include paths it needed are
fixed. What is left is one identifier: it calls `vTaskDelay()` twice, and
`include/freertos/task.h` in this port is a type-only shim that does not
declare it. `portTICK_PERIOD_MS`, which the second call also uses, is already
in `include/freertos/FreeRTOS.h`.

This is deliberate on the shim's part — its own comment says a compile error
naming the missing thing is better than a stub that pretends — so the decision
belongs to whoever owns those files. The port already has the implementation:
`rtems_wifi_task_delay()` in `src/rtems_wifi_os_adapter.c` is
`rtems_task_wake_after(tick)`. `eloop.c` cannot reach it, because it calls
FreeRTOS directly rather than through `wifi_osi_funcs_t`.

With that one declaration supplied, `eloop.c` compiles unmodified, the link
reports zero undefined symbols, `nm` shows `esp_supplicant_init` as `T` and
`g_wifi_default_wpa_crypto_funcs` as `R`, and the image reaches
`esp_wifi_init returned 0` with the
`esp_supplicant_init is not linked in` line gone.

## Still skipped, and why that is fine

`tools/supplicant-build.sh` compiles 130 of 147 sources. The 17 it does not are
DPP, Wi-Fi Aware, PASN, the 802.11k/v roaming files, the four
`crypto_mbedtls*.c`, `tls_mbedtls.c`, the two hardware-SHA PBKDF2 accelerators,
and `eloop.c`. Every one but `eloop.c` corresponds to a feature that is off
above.

Skipping stays safe for the same reason it did in step 5: the link is the
arbiter. A source skipped that is actually needed shows up as an undefined
symbol, not as silence — which is exactly how the eight of the previous section
were found.
