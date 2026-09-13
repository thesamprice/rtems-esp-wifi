/* SPDX-License-Identifier: Apache-2.0 */

/*
 * mbedtls configuration for the RTEMS ESP32-C3 WiFi port.
 *
 * This file is BOTH config files mbedtls 4.x wants.  The 4.x tree is split in
 * two -- tf-psa-crypto underneath, mbedtls (TLS and X.509) on top -- and each
 * half reads its own file:
 *
 *   -DTF_PSA_CRYPTO_CONFIG_FILE='<rtems-esp/mbedtls_config.h>'
 *   -DMBEDTLS_CONFIG_FILE='<rtems-esp/mbedtls_config.h>'
 *
 * tf-psa-crypto/include/tf-psa-crypto/build_info.h reads the first and
 * include/mbedtls/build_info.h reads the second, in that order.  The include
 * guard below means the second read is a no-op, which is what is wanted: every
 * macro is already set.  Espressif's own port does the same thing with one
 * esp_config.h.
 *
 * These are the *whole* configuration, not an overlay.  Passing
 * TF_PSA_CRYPTO_CONFIG_FILE replaces psa/crypto_config.h rather than adding to
 * it, so nothing is on that is not written here.  The alternative --
 * TF_PSA_CRYPTO_USER_CONFIG_FILE, which is what ESP-IDF uses -- starts from
 * upstream's default, which enables RSA, ECDSA, seven curves, ChaCha20-Poly1305
 * and the rest, and then #undefs back down.  For a station that speaks WPA2-PSK
 * and WPA3-SAE the deny-list is much longer than the allow-list, and every
 * entry that gets missed is silent flash.
 *
 * Why not ESP-IDF's esp_config.h directly: it hard-wires the ESP hardware PSA
 * drivers on capability rather than on configuration --
 *
 *     #ifdef SOC_HMAC_SUPPORTED
 *     #define ESP_HMAC_OPAQUE_DRIVER_ENABLED
 *     #endif
 *
 * -- and SOC_HMAC_SUPPORTED is true for the C3, so there is no Kconfig setting
 * that turns it off.  That driver needs the HMAC peripheral, eFuse block
 * reads and esp_security's init, none of which this port has.  Everything
 * here is the software implementation.
 *
 * === What the supplicant actually asks for ================================
 *
 * Measured, not assumed: compile the translation units in
 * tools/supplicant-build.sh, link, and read the undefined PSA and mbedtls
 * symbols.  They come from two places.
 *
 * The crypto function table the WiFi libraries call through
 * (src/crypto/crypto_ops.c, g_wifi_default_wpa_crypto_funcs):
 *
 *   hmac_sha256_vector  HMAC-SHA-256, the EAPOL-Key MIC and the KDF for CCMP
 *   sha256_vector       SHA-256
 *   pbkdf2_sha1         passphrase -> PSK; HMAC-SHA-1 and so PKCS#5
 *   aes_128_cbc_encrypt AES-128-CBC, the EAPOL-Key Key Data encryption
 *   aes_128_cbc_decrypt
 *   omac1_aes_128       AES-CMAC, the 802.11w management-frame MIC
 *   ccmp_encrypt        AES-CCM, the pairwise cipher
 *   ccmp_decrypt
 *   aes_wrap            AES key wrap (RFC 3394), the group key in message 3
 *   aes_unwrap
 *   aes_gmac            only under CONFIG_GMAC, which is off; see below
 *
 * And WPA3-SAE (src/common/sae.c through crypto_mbedtls-ec.c), which needs
 * ECC point and scalar arithmetic over one curve and SHA-256.
 *
 * So: AES with four modes, two hashes, HMAC, CMAC, CCM, key wrap, PKCS#5,
 * one elliptic curve.  Nothing else.
 */

#ifndef RTEMS_ESP_MBEDTLS_CONFIG_H
#define RTEMS_ESP_MBEDTLS_CONFIG_H

/*
 * === the PSA half ======================================================
 *
 * The supplicant's crypto_mbedtls.c is written against the PSA API for
 * everything symmetric, so the PSA core is not optional here even though a
 * WPA2 station needs no key store: psa_hash_*, psa_mac_*, psa_cipher_* and
 * psa_aead_* are the calls, and MBEDTLS_PSA_CRYPTO_C is what implements them
 * rather than only declaring the client side.
 */
#define MBEDTLS_PSA_CRYPTO_C

/*
 * Randomness.
 *
 * MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG replaces mbedtls' entropy accumulator and
 * CTR_DRBG with one callback, mbedtls_psa_external_get_random().  ESP-IDF's
 * components/mbedtls/port/esp_hardware.c is that callback and this port
 * compiles it unchanged -- it is esp_fill_random(), which
 * src/rtems_esp_glue.c implements as getentropy().
 *
 * Be clear about what that means rather than letting the module list imply
 * something stronger.  On this BSP getentropy() is
 * bsps/shared/dev/getentropy/getentropy-cpucounter.c, a CPU counter, so the
 * PSA random source is a CPU counter: SAE's scalar and mask and the
 * supplicant's SNonce are predictable to anyone who knows the boot timing.
 * Enabling MBEDTLS_CTR_DRBG_C would *not* fix that -- a DRBG whitens a weak
 * seed, it does not add entropy -- it would only cost 6 KiB of flash and make
 * the weakness harder to see.  The fix is a BSP with a real entropy source,
 * and getentropy() is the seam where it arrives; nothing here changes.
 *
 * ESP-IDF defines both of these and so does this file: esp_hardware.c guards
 * mbedtls_psa_external_get_random() with MBEDTLS_PSA_DRIVER_GET_ENTROPY rather
 * than with MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG, so with only the latter defined
 * the callback is compiled out and the link fails on it.
 */
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
#define MBEDTLS_PSA_DRIVER_GET_ENTROPY

/*
 * No persistent keys.  MBEDTLS_PSA_CRYPTO_STORAGE_C is upstream's default and
 * it needs an ITS backend -- MBEDTLS_PSA_ITS_FILE_C, which needs MBEDTLS_FS_IO
 * and a writable file system.  Every key the supplicant makes is volatile and
 * dies with the association, so this is off and psa_crypto_storage.c and
 * psa_its_file.c are not built.
 */

/* Hashes. */
#define PSA_WANT_ALG_SHA_256                    1
/*
 * SHA-1 is here for PBKDF2 only.  WPA2-PSK derives the 256-bit PSK from the
 * passphrase with PBKDF2-HMAC-SHA-1, 4096 iterations, the SSID as salt
 * (IEEE 802.11i Annex J), so a station that takes a passphrase cannot avoid
 * it.  It is not used to sign or verify anything.
 */
#define PSA_WANT_ALG_SHA_1                      1

/*
 * HMAC.  hmac_sha256_vector is the EAPOL-Key MIC for the AKMs a C3 station
 * uses and the PRF that expands the PMK into the PTK.  PSA models HMAC as a
 * MAC over a key of type HMAC, so both lines are needed.
 */
#define PSA_WANT_ALG_HMAC                       1
#define PSA_WANT_KEY_TYPE_HMAC                  1

/* AES, and only AES: no ARIA, Camellia, ChaCha20, DES or 3DES. */
#define PSA_WANT_KEY_TYPE_AES                   1

/*
 * The four AES modes, one caller each.
 *
 *   ECB  the raw block operation.  crypto_mbedtls.c's aes_encrypt()/
 *        aes_decrypt() are a one-block ECB cipher operation, and everything
 *        that builds its own mode on top of a block -- aes-siv, the internal
 *        CTR -- goes through them.
 *   CBC  aes_128_cbc_encrypt/decrypt, the Key Data field of EAPOL-Key
 *        message 3 when the key descriptor version says AES.
 *   CTR  crypto_mbedtls.c's aes_ctr_encrypt().
 *   CCM  the CCMP pairwise cipher, through src/crypto/ccmp.c.
 *
 * No GCM: it is 802.11 GCMP, which is CONFIG_GCMP/CONFIG_GMAC in the
 * supplicant and off in tools/supplicant-build.sh, and with it off
 * crypto_ops.c compiles esp_aes_gmac() to `return -1`.  The C3's blobs do not
 * offer GCMP to an AP.  It is the single largest module a WPA2 station can
 * decline: gcm.c plus the PSA AEAD glue for it.
 *
 * No XTS, CFB, OFB, CBC-PKCS7, ECB-with-padding: nothing calls them.
 */
#define PSA_WANT_ALG_ECB_NO_PADDING             1
#define PSA_WANT_ALG_CBC_NO_PADDING             1
#define PSA_WANT_ALG_CTR                        1
#define PSA_WANT_ALG_CCM                        1

/* AES-CMAC: omac1_aes_128, the BIP MIC for protected management frames. */
#define PSA_WANT_ALG_CMAC                       1

/*
 * === WPA3-SAE ==========================================================
 *
 * SAE is a dragonfly PAKE over an elliptic curve, so the station needs real
 * curve arithmetic -- point multiply, point add, square roots mod p -- and not
 * just a signature primitive.  crypto_mbedtls-ec.c reaches for the legacy
 * mbedtls_ecp_* and mbedtls_mpi_* API to get it; PSA has no interface for
 * "multiply this point by this scalar".
 *
 * In 4.x the legacy modules are not configured directly.  MBEDTLS_ECP_C and
 * MBEDTLS_BIGNUM_C are set by
 * mbedtls/private/crypto_adjust_config_enable_builtins.h as a consequence of
 * asking for a curve mechanism, so the way to get ecp.c and bignum.c is to
 * declare the PSA mechanism that needs them.  PSA_WANT_ALG_ECDH is that
 * declaration: it pulls in ECP_C, BIGNUM_C and the key-pair types, and it is
 * also genuinely used -- crypto_mbedtls-ec.c does the SAE shared secret with
 * psa_raw_key_agreement().
 *
 * One curve.  Group 19, secp256r1, is the only group a C3 station offers and
 * the only one hostapd requires an SAE implementation to support.  Each extra
 * curve is its own constant table in ecp_curves.c, which is the 270 KiB source
 * in the tree, so this is where the biggest single saving is: groups 20 and 21
 * (secp384r1, secp521r1) and the Brainpool and Curve25519 families are all
 * off.  Adding one is one line here and a rebuild.
 */
#define PSA_WANT_ALG_ECDH                       1
#define PSA_WANT_ECC_SECP_R1_256                1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_BASIC    1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT   1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_EXPORT   1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_GENERATE 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_DERIVE   1
#define PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY        1

/*
 * MBEDTLS_ECP_NIST_OPTIM is the fast reduction for the NIST primes.  It is
 * not a module, it is a different modulo path inside ecp_curves.c, and it is
 * on because SAE runs point multiplication on the association critical path
 * and the generic Montgomery reduction is several times slower.
 */
#define MBEDTLS_ECP_NIST_OPTIM

/*
 * MBEDTLS_ALLOW_PRIVATE_ACCESS.  crypto_mbedtls-ec.c and
 * crypto_mbedtls-bignum.c reach into mbedtls_mpi and mbedtls_ecp_point
 * fields, so this is required and not a preference -- without it those two
 * files do not compile.  crypto_mbedtls.c #defines it itself at the top;
 * the other two rely on the config, which is why it is here.  Espressif's
 * esp_config.h has it on its first line for the same reason.
 */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

/*
 * === the cost of CONFIG_ECC ============================================
 *
 * These six are not wanted by anything on the association path.  They are
 * here because crypto_mbedtls-ec.c compiles five DER helpers --
 * crypto_ec_get_priv_key_der(), crypto_write_pubkey_der(),
 * crypto_ec_key_parse_priv(), crypto_ec_parse_subpub_key() and
 * crypto_pk_write_formatted_pubkey_der() -- inside its one big
 * `#ifdef CONFIG_ECC` with no second guard, and only DPP and SAE-PK ever call
 * them.  SAE needs CONFIG_ECC, so the file is in, so the references are in,
 * so the modules are in.
 *
 * mbedtls_pk_write_key_der(), mbedtls_pk_parse_key() and
 * mbedtls_asn1_write_*() are PK and ASN.1; mbedtls_ecdsa_der_to_raw() is
 * declared behind PSA_HAVE_ALG_SOME_ECDSA, which is why a station that
 * verifies no signatures still has to ask for ECDSA to get a DER-to-raw
 * converter.
 *
 * Compiling the supplicant with -ffunction-sections and linking with
 * --gc-sections would drop all five and with them these modules.  That is not
 * done here: --gc-sections can also discard a section whose undefined symbol
 * was never resolved, so it would turn tools/applink.sh's unresolved-symbol
 * count -- the only check that the port is actually complete -- into a number
 * that cannot be trusted.  Paying the flash is the cheaper mistake.
 */
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define PSA_WANT_ALG_ECDSA                      1

/*
 * === support modules ===================================================
 *
 * PKCS#5 for pbkdf2_sha1.  crypto_mbedtls.c calls
 * mbedtls_pkcs5_pbkdf2_hmac_ext(), the legacy entry point, not PSA's
 * PSA_ALG_PBKDF2_HMAC key derivation -- so PSA_WANT_ALG_PBKDF2_HMAC would not
 * satisfy it and is left off.  MBEDTLS_PKCS5_C brings MBEDTLS_MD_C with it
 * through crypto_adjust_config_support.h, which is what pkcs5.c uses for the
 * HMAC iteration.
 */
#define MBEDTLS_PKCS5_C

/*
 * AES key wrap, RFC 3394.  aes_wrap()/aes_unwrap() in the crypto table are
 * mbedtls_nist_kw_wrap()/unwrap(), and this is the group key travelling in
 * EAPOL-Key message 3 -- so it is on the plain WPA2-PSK path, not an extra.
 * nist_kw.c lives in tf-psa-crypto/extras in 4.x and needs MBEDTLS_CIPHER_C,
 * which crypto_adjust_config_tweak_builtins.h derives from it.
 */
#define MBEDTLS_NIST_KW_C

/*
 * === deliberately absent ===============================================
 *
 * No TLS and no X.509.  MBEDTLS_SSL_TLS_C, MBEDTLS_X509_USE_C and everything
 * under them are unset, so library/ contributes nothing: ssl_tls.c,
 * ssl_msg.c, x509_crt.c and the rest are never referenced.  WPA2-Enterprise
 * (EAP-TLS, PEAP, TTLS) is what would need them, and this port builds the
 * PSK and SAE AKMs only -- see docs/step6-supplicant.md for what that leaves
 * out.  Half of mbedtls by source size is behind this one decision.
 *
 * No RSA, no ECDSA, no public-key parsing.  MBEDTLS_RSA_C, MBEDTLS_PK_C,
 * MBEDTLS_PK_PARSE_C and PSA_WANT_ALG_ECDSA are all a certificate or a
 * signature away, and neither WPA2-PSK nor SAE has either.  SAE-PK and DPP
 * would need ECDSA; both are off.
 *
 * No MD5, no SHA-384/512, no SHA-3, no RIPEMD-160.  MD5 is MSCHAPv2, which is
 * EAP.  SHA-384 is the Suite-B AKMs and SAE groups 20 and 21.  The supplicant
 * substitutes its own software sha1-internal.c and md5-internal.c when the
 * mbedtls ones are absent, so leaving these off does not create a hole -- it
 * moves the code, and only for AKMs this build does not offer.  SHA-256 and
 * SHA-1 are enabled above and are all that the PSK and SAE paths reach.
 *
 * No self-tests, no error strings, no debug.  MBEDTLS_SELF_TEST,
 * MBEDTLS_ERROR_C and MBEDTLS_DEBUG_C are string tables and test vectors;
 * failures here surface as a printk from the supplicant with the numeric PSA
 * status, which src/rtems_esp_glue.c already routes to the console.
 *
 * No threading.  MBEDTLS_THREADING_C exists to lock the shared key store and
 * the shared RNG state.  With MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG there is no RNG
 * state, and every PSA key here is created and destroyed inside one call from
 * the WiFi task.  This is the one entry worth revisiting if anything else in
 * the image starts using PSA: two tasks sharing the key store without it is a
 * data race, not a slowdown.
 */

/*
 * No MBEDTLS_HAVE_TIME.  Certificate validity is the only thing mbedtls wants
 * a clock for and there are no certificates here.  The supplicant's own
 * timestamps come from os_get_time() in port/os_xtensa.c.
 */

#endif /* RTEMS_ESP_MBEDTLS_CONFIG_H */
