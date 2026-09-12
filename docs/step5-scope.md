# Step 5: what the station path actually needs

Step 4 left seven symbols. This is what each turns out to be, measured rather
than guessed, because two plausible readings of that list are both wrong.

## The first wrong reading: "five of the six are mesh, so drop mesh"

`nm` says five of the six `esp_*` symbols are referenced only by `libmesh.a`
and `libsmartconfig.a`. A station needs neither, so dropping those archives
looks free.

It is not. `libnet80211.a` and `libpp.a` reference mesh and espnow symbols
themselves:

```
g_mt  mt_get_peer_info  g_espnow_user_oui
ieee80211_init_mesh_assoc_ie  ieee80211_vnd_mesh_quick_{get,set}
ieee80211_vnd_mesh_roots_{get,set}  mesh_{get,set}_parent_candidate
mesh_{get,set}_parent_monitor_config  mesh_{get,set}_rssi_threshold
mesh_clear_parent_candidate  mesh_set_ie_crypto_config
mesh_sta_auth_expire_time
```

So the archives are mutually entangled and the whole set is required. There is
no station-only subset. `tools/applink.sh` with mesh, espnow, smartconfig and
wapi removed reports 19 unresolved, up from 7.

## The second wrong reading: "compile ESP-IDF's wifi_init.c"

`esp_wifi/src/wifi_init.c` is in the branch, so it looks like the six `esp_*`
symbols are a compile away. Five of its includes are not in the branch at all:

| header | component |
|---|---|
| `esp_pm.h`, `esp_private/pm_impl.h` | power management |
| `esp_psram.h` | PSRAM |
| `hal/adc_types.h` | ADC |
| `esp_wpa.h` | supplicant's public header |

`esp-hal-3rdparty` ships `components/hal` but not its ADC headers, and ships
`esp_pm` and `esp_psram` as directories without what `wifi_init.c` asks for.
So the file cannot be compiled from this branch, and chasing the headers into
esp-idf proper is exactly the "don't bring the whole HAL in" that the issue
asked us to avoid.

The port writes its own `esp_wifi_init` instead, which is what NuttX does
(`arch/risc-v/src/common/espressif/esp_wifi_api.c`).

## What is actually left, and where each piece comes from

Reading ESP-IDF's `esp_wifi_init` with PM, coex, PSRAM, MAC_BB_PD, NAN,
roaming and tickless idle all off, it reduces to nine calls. Seven of them are
in `libnet80211.a` already:

```
esp_wifi_set_sleep_min_active_time        libnet80211.a
esp_wifi_set_keep_alive_time              libnet80211.a
esp_wifi_set_sleep_wait_broadcast_data_time  libnet80211.a
esp_wifi_internal_set_log_level           libnet80211.a
esp_wifi_init_internal                    libnet80211.a
esp_wifi_connect_internal                 libnet80211.a
esp_wifi_disconnect_internal              libnet80211.a
```

`esp_wifi_connect` and `esp_wifi_disconnect` are therefore one-line wrappers;
ESP-IDF's are one line each too, the rest of their bodies being the roaming
app.

Two pieces are genuinely new, and both are available:

**PHY.** `register_chipv7_phy(init_data, cal_data, cal_mode)` is in
`libphy.a`, and the C3's `esp_phy/esp32c3/phy_init_data.c` ships the init
data as source. Calling it with `PHY_RF_CAL_FULL` calibrates from scratch,
**so NVS is not on the critical path.** NVS in ESP-IDF only caches calibration
to shorten boot; the 12 NVS entries in the adapter table can stay named
failures for now, which is worth knowing because NVS was the largest single
group of them.

**The supplicant.** `esp_supplicant_init()` is what `esp_wifi_init` calls for
WPA2, and `hexstr2bin` -- the seventh symbol -- is in the same component.
`wpa_supplicant` **is** one of the branch's components; it simply has not been
fetched yet. So this is a fetch and a compile, not new code.

## Order of work

1. Fetch `wpa_supplicant`; that closes `hexstr2bin`.
2. `src/rtems_esp_wifi_init.c`: `esp_wifi_init`, `esp_wifi_connect`,
   `esp_wifi_disconnect`, PHY registration with full calibration.
3. `esp_event`, also in the branch, for `esp_event_handler_register`,
   `esp_event_handler_unregister` and `esp_mesh_send_event_internal`.
4. The netif joining it to rtems-lwip -- `s_netstack_free` and
   `g_net80211_tx_func` are the seam, and `include/esp_netif.h` is where the
   placeholder is.
