#include <zephyr/sys/util.h>
/* SPDX-License-Identifier: MIT
 * .ast signature verification. See aster_sig.h for the layout contract.
 */
#include "aster_sig.h"
#include "aster_pkg.h"
#include "aster_crypto.h"
K_MUTEX_DEFINE(aster_crypto_lock);
/* Outside the CONFIG_ASTER_SIGNATURE_VERIFY block below on purpose: the header
 * stays available whichever policy is compiled in, so a change of branch can
 * never turn into a surprise build break. */
#include "aster_bytes.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <aster/aster.h>

LOG_MODULE_REGISTER(aster_sig, LOG_LEVEL_INF);

#if IS_ENABLED(CONFIG_ASTER_SIGNATURE_VERIFY)
#include <psa/crypto.h>

static int hexnib(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

/* Decode CONFIG_ASTER_VENDOR_PUBKEY (130 hex chars) -> 65 bytes. */
static int load_pubkey(uint8_t out[65])
{
	const char *hex = CONFIG_ASTER_VENDOR_PUBKEY;
	for (int i = 0; i < 65; i++) {
		int hi = hexnib(hex[2 * i]), lo = hexnib(hex[2 * i + 1]);
		if (hi < 0 || lo < 0) {
			return -1;
		}
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return out[0] == 0x04 ? 0 : -1;
}

static int psa_verify(const uint8_t *region, size_t rlen, const uint8_t sig[64])
{
	static bool inited;
	if (!inited) {
		if (psa_crypto_init() != PSA_SUCCESS) {
			LOG_ERR("psa init failed");
			return ASTER_EIO;
		}
		inited = true;
	}
	uint8_t pub[65];
	if (load_pubkey(pub) != 0) {
		LOG_ERR("bad CONFIG_ASTER_VENDOR_PUBKEY");
		return ASTER_EINVAL;
	}
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);
	/* PSA enforces BOTH a usage flag AND an allowed-algorithm policy.
	 * Forgetting psa_set_key_algorithm is the classic -133 (NOT_PERMITTED)
	 * trap: the key imports fine, but every psa_verify_hash is refused. */
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_key_id_t key;
	psa_status_t ist = psa_import_key(&attr, pub, sizeof(pub), &key);
	if (ist != PSA_SUCCESS) {
		LOG_ERR("pubkey import: %d", (int)ist);
		return ASTER_EINVAL;
	}
	uint8_t hash[32];
	size_t hlen = 0;
	psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, region, rlen, hash,
					   sizeof(hash), &hlen);
	int rc = ASTER_ESIGN;
	if (st == PSA_SUCCESS && hlen == 32) {
		LOG_DBG("verify rlen=%u hash=%02x%02x%02x%02x", (unsigned)rlen,
			hash[0], hash[1], hash[2], hash[3]);
		st = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, hlen,
				     sig, ASTER_SIG_LEN);
		if (st != PSA_SUCCESS) {
			LOG_WRN("signature rejected: psa=%d", (int)st);
		}
		rc = (st == PSA_SUCCESS) ? ASTER_OK : ASTER_ESIGN;
	} else if (st != PSA_SUCCESS) {
		LOG_ERR("hash: %d", (int)st);
		rc = ASTER_EIO;
	}
	psa_destroy_key(key);
	return rc;
}
#endif /* CONFIG_ASTER_SIGNATURE_VERIFY */

int aster_sig_check(const uint8_t *img, size_t size)
{
#if !IS_ENABLED(CONFIG_ASTER_SIGNATURE_VERIFY)
	/* Policy OFF: no verification compiled in; installer never calls here. */
	ARG_UNUSED(img);
	ARG_UNUSED(size);
	return ASTER_OK;
#else
	if (size < ASTER_PKG_HDRSIZE) {
		return ASTER_ECORRUPT;
	}
	uint32_t flags = aster_le32(img + 8);
	if (!(flags & ASTER_FLAG_SIGNED)) {
#if IS_ENABLED(CONFIG_ASTER_SIGNATURE_REQUIRE)
		return ASTER_EUNSIGNED;
#else
		return ASTER_OK; /* allow-unsigned */
#endif
	}
	if (size < (size_t)ASTER_PKG_HDRSIZE + ASTER_SIG_LEN) {
		return ASTER_ECORRUPT; /* claims signed but too small */
	}
	const uint8_t *sig = img + size - ASTER_SIG_LEN;
	size_t rlen = size - ASTER_SIG_LEN;
	k_mutex_lock(&aster_crypto_lock, K_FOREVER);
	int result = psa_verify(img, rlen, sig);
	k_mutex_unlock(&aster_crypto_lock);
	return result;
#endif
}
