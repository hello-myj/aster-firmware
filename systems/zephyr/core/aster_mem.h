/* SPDX-License-Identifier: MIT
 * ASTER core memory: PSRAM-backed allocation for WAMR pool, wasm images,
 * surface buffers; internal DRAM via the libc tail heap for stacks and small hot objects.
 */
#ifndef ASTER_MEM_H
#define ASTER_MEM_H

#include <stdbool.h>
#include <stddef.h>

void *aster_psram_alloc(size_t size);
void  aster_psram_free(void *ptr);
/* Leftover internal DRAM (the libc heap after .bss), not k_malloc and not
 * PSRAM. Use for objects that must stay reachable while the flash cache is
 * off, and that should disappear again when the caller is finished. */
void *aster_dram_alloc(size_t size);
void  aster_dram_free(void *ptr);
void  aster_core_init(void);

/* Where the external (PSRAM) memory actually is, as the linker laid it out.
 *
 * Sizes declared in Kconfig (`CONFIG_ASTER_RAM_EXTERNAL`) say what the board
 * is supposed to have; these say what the image was actually built against.
 * The two are separate facts and a port can get them out of step, so both
 * are reportable — see the mismatch check in `aster sys mem`.
 *
 * Filled from the linker script's own boundary symbols, the same ones
 * sdk/tools/mem_ledger.py reads, so the device and docs/memory.md cannot
 * drift apart by re-deriving the numbers differently.
 *
 * Returns false (and zeroes *out) when the port exports no such symbols —
 * a port without external RAM, or one that names them differently. */
typedef struct {
	size_t chip;      /* what the part has, i.e. CONFIG_ASTER_RAM_EXTERNAL */
	size_t total;     /* first usable byte -> end of the heap window */
	size_t static_sz; /* non-heap bytes before the heap (stacks, pools, .bss) */
	size_t heap;      /* the external heap handed to shared_multi_heap */
} aster_psram_geom_t;

bool aster_psram_geom_get(aster_psram_geom_t *out);

#endif
