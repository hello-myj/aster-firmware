/* SPDX-License-Identifier: MIT
 * .mja package signature verification (ECDSA-P256 / SHA-256).
 *
 * Layout: when header.flags bit0 (ASTER_FLAG_SIGNED) is set, the LAST 64 bytes
 * of the file are a raw r||s ECDSA signature. The signed region is every byte
 * BEFORE that block -- i.e. [0, size-64) -- which includes the 64-byte header
 * (with the flag already set), manifest, wasm, aot and icon. Because the
 * signature block sits at the tail, nothing inside the hashed span references
 * its own location, so there is no self-referential field to zero out.
 *
 * The trust anchor is CONFIG_ASTER_VENDOR_PUBKEY (uncompressed P-256 point).
 */
#ifndef ASTER_SIG_H
#define ASTER_SIG_H

#include <stdint.h>
#include <stddef.h>
#include <aster/error.h>

/* Header flags (offset 8). */
#define ASTER_FLAG_SIGNED 0x1u
/* bit1 reserved: was HAS_UI (UIB1 ui.bin), dropped in ABI 3.0 */

/* Policy from Kconfig: 0 off, 1 allow-unsigned, 2 require-signed */
#define ASTER_SIG_POLICY_OFF     0u
#define ASTER_SIG_POLICY_ALLOW   1u
#define ASTER_SIG_POLICY_REQUIRE 2u

#define ASTER_SIG_LEN 64u

/* Verify an in-memory package image. Returns ASTER_OK / ASTER_ESIGN /
 * ASTER_EUNSIGNED / ASTER_ECORRUPT (see aster_sig.h top comment + docs). */
int aster_sig_check(const uint8_t *img, size_t size);

#endif
