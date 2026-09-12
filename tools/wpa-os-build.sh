#!/bin/sh
# wpa_supplicant's OS layer.  Despite the file name, port/os_xtensa.c is not
# Xtensa-specific: it is gettimeofday, sleep and the two esp_random calls, all
# of which work here.  It is the branch's own file rather than a rewrite, so it
# cannot drift from what the rest of the supplicant expects.
#
# It needs one mbedtls header, for a forced_memzero() that is behind
# CONFIG_CRYPTO_MBEDTLS and so not compiled here -- the #include is
# unconditional even though the use is not.  mbedtls is a gitlink in the branch
# with no .gitmodules to give it a URL, the same as the WiFi blobs, so it comes
# from espressif/mbedtls at ce3f3485a121c100f58f36d700cb35b060f6e866 and is
# wanted in full for the supplicant anyway.
SP=${SP:?set SP to the work directory}
LIB=$SP/iram-prefix/riscv-rtems7/esp32c3db/lib
HAL=$SP/hal/components
export PATH=$HOME/rtems/7/bin:$PATH
riscv-rtems7-gcc -march=rv32imc -mabi=ilp32 -DESP_PLATFORM -isystem "$LIB/include" \
  -I $SP/glue/include/rtems-esp -I $SP/glue/include \
  -I $HAL/wpa_supplicant/port/include -I $HAL/wpa_supplicant/src \
  -I $SP/mbedtls/tf-psa-crypto/include -I $SP/mbedtls/include \
  -I $HAL/wpa_supplicant/src/utils -I $HAL/wpa_supplicant/include \
  -I $HAL/esp_wifi/include -I $HAL/esp_common/include -I $HAL/log/include \
  -I $HAL/esp_event/include -I $HAL/esp_rom/include \
  -I $HAL/soc/include -I $HAL/soc/esp32c3/include \
  -I $HAL/esp_hw_support/include -I $HAL/esp_timer/include \
  -c -o $SP/wpa_os.o $HAL/wpa_supplicant/port/os_xtensa.c 2>&1
