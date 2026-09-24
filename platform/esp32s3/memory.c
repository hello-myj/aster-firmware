/* SPDX-License-Identifier: MIT */
#include "aster_mem.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/multi_heap/shared_multi_heap.h>

LOG_MODULE_REGISTER(aster_mem, LOG_LEVEL_INF);

#define ASTER_ALIGN 16

void *aster_psram_alloc(size_t size)
{
	if (size == 0) {
		return NULL;
	}
	return shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL, ASTER_ALIGN, size);
}

void aster_psram_free(void *ptr)
{
	if (ptr) {
		shared_multi_heap_free(ptr);
	}
}

/* The linker script's external-RAM boundary symbols. Declared weak on
 * purpose: a port that does not define them links with these as zero and
 * aster_psram_geom_get() reports "unavailable" rather than failing the build
 * -- the same form the SoC port's own PSRAM driver uses for these absolute
 * symbols. */
extern int _ext_ram_start[] __attribute__((weak));
extern int _ext_ram_heap_start[] __attribute__((weak));
extern int _ext_ram_heap_end[] __attribute__((weak));

bool aster_psram_geom_get(aster_psram_geom_t *out)
{
	if (!out) {
		return false;
	}
	*out = (aster_psram_geom_t){ 0 };
	if ((uintptr_t)_ext_ram_start == 0 || (uintptr_t)_ext_ram_heap_start == 0 ||
	    (uintptr_t)_ext_ram_heap_end == 0) {
		return false;
	}
	uintptr_t start = (uintptr_t)_ext_ram_start;
	uintptr_t heap_start = (uintptr_t)_ext_ram_heap_start;
	uintptr_t heap_end = (uintptr_t)_ext_ram_heap_end;

	if (heap_start < start || heap_end < heap_start) {
		return false;
	}
	out->chip = CONFIG_ASTER_RAM_EXTERNAL;
	out->static_sz = heap_start - start;
	out->heap = heap_end - heap_start;
	out->total = heap_end - start;
	return true;
}

void aster_core_init(void)
{
	/* shared_multi_heap regions are registered by the ESP PSRAM driver */
}
