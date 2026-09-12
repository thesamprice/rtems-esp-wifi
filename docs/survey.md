# Reproducing the survey

Every number in the README comes from one of these. They need only the RTEMS
RISC-V toolchain and network access; nothing is built.

## Which branch

```sh
gh api repos/espressif/esp-hal-3rdparty --jq '.default_branch'
gh api "repos/espressif/esp-hal-3rdparty/branches?per_page=100" --jq '.[].name'
gh api repos/espressif/esp-hal-3rdparty/contents/README.md --jq '.content' \
    | base64 -d
```

`main` holds only `tools/` and the README. The README names `sync/master.c` as
the sole active branch and lists the rest as deprecated. The component list for
it is `tools/sync_master.c.txt`.

## The blobs are a separate repository

```sh
gh api "repos/espressif/esp-hal-3rdparty/contents/components/esp_wifi/lib?ref=sync/master.c" \
    --jq '.type'
```

Returns `submodule`. The blobs are `espressif/esp32-wifi-lib`, `esp32c3/`:
`libcore.a`, `libespnow.a`, `libmesh.a`, `libnet80211.a`, `libpp.a`,
`libsmartconfig.a`, `libwapi.a`.

## What they require

```sh
for f in libcore.a libnet80211.a libpp.a; do
  curl -sLO "https://raw.githubusercontent.com/espressif/esp32-wifi-lib/master/esp32c3/$f"
done

riscv-rtems7-nm -A --undefined-only lib*.a | awk '{print $NF}' | sort -u > undef.txt
riscv-rtems7-nm -A --defined-only   lib*.a | awk '{print $NF}' | sort -u > def.txt
comm -23 undef.txt def.txt          # 163 external
grep -c '^__' -                     # 27 of those are compiler runtime
```

Reading the counts rather than the header matters: the header declares 125
function pointers and 125 symbols remain after removing the runtime and libc,
and the two sets are **not** the same. `categorised.txt` in this directory has
the full list grouped.

## Footprint

```sh
riscv-rtems7-size -A libnet80211.a libpp.a libcore.a
```

Sum by section prefix: `.bss`/`.noinit` 9.8 KiB, `.data`/`.dram` 3.6 KiB,
anything with `iram` in the name 40.5 KiB, `.text` 252.1 KiB, `.rodata`
48.9 KiB.

The `iram` total is the one that matters, because it is code that cannot
execute from flash.

## What the part has spare

From the rtems-esphome tree, which reports demand separately from what is left
over:

```sh
python3 scripts/footprint.py \
    tests/rtems-ci/.esphome/build/reference-node/reference-node.elf
```

`RAM, sized to fit  289.2 KiB  .work` — that is the heap and workspace left
after an ESPHome node, and it is what the blobs would come out of.

## The NuttX precedent

```sh
gh api "search/code?q=wifi_osi_funcs+repo:apache/nuttx" --jq '.items[].path'
gh api "repos/apache/nuttx/contents/arch/risc-v/src/esp32c3/esp_wifi_adapter.c" \
    --jq '.size'
```

83004 bytes. The common files are in
`arch/risc-v/src/common/espressif/`: `esp_wifi_api.c`,
`esp_wifi_event_handler.c`, `esp_wifi_utils.c`, `esp_timer_adapter.c`.
