/* SPDX-License-Identifier: MIT
 * This Zephyr crypto build has no Mbed TLS threading backend. Serialize the
 * global PSA key store/allocator across package verification and TLS calls.
 * Never hold a network/API mutex while acquiring this lock.
 */
#ifndef ASTER_CRYPTO_H
#define ASTER_CRYPTO_H
#include <zephyr/kernel.h>
extern struct k_mutex aster_crypto_lock;
#endif
