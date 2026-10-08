/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-FileCopyrightText: 2024 Siemens AG (For Zephyr usermode changes)
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
/*
 * AOT text in PSRAM does not use CONFIG_SPIRAM_FETCH_INSTRUCTIONS. That
 * switch copies the whole firmware out of flash and remaps instruction
 * fetch; this Zephyr image still executes from flash and dies at that
 * remap. The guest allocation only needs the existing PSRAM heap plus
 * the instruction-bus alias.
 *
 * Flash and octal PSRAM share one MSPI. Display DMA stays on the
 * internal-RAM bounce (CONFIG_SPI_ESP32_TX_PSRAM_DIRECT=n), so GDMA
 * does not sit on that bus while the instruction cache fills AOT
 * lines. Flash programming still disables the cache with interrupts
 * off, on this single core, and does not turn on the
 * "cache stays enabled during flash" mode.
 */
#if defined(CONFIG_SOC_SERIES_ESP32S3) && defined(CONFIG_ESP_SPIRAM)
#define XAPP_AOT_PSRAM 1
#include <zephyr/multi_heap/shared_multi_heap.h>
#include <soc.h>
#include <esp_cache.h>
#endif

/* function pointers for executable memory management */
static exec_mem_alloc_func_t exec_mem_alloc_func = NULL;
static exec_mem_free_func_t exec_mem_free_func = NULL;

/*
 * ESP32-S3 maps the same physical SRAM as DRAM (0x3FC…) and IRAM (0x403…).
 * AOT must execute via IRAM; writes/relocs go through the DRAM mirror.
 */
#if defined(CONFIG_SOC_SERIES_ESP32S3)
#define XAPP_IRAM_DRAM_OFFSET 0x006f0000u
#define XAPP_IRAM_VADDR_MIN   0x40370000u
#define XAPP_IRAM_VADDR_MAX   0x40400000u

static bool
xapp_addr_in_iram(const void *p)
{
	uintptr_t a = (uintptr_t)p;

	return a >= XAPP_IRAM_VADDR_MIN && a < XAPP_IRAM_VADDR_MAX;
}

static void *
xapp_dram_to_iram(void *dram)
{
	return (void *)((uintptr_t)dram + XAPP_IRAM_DRAM_OFFSET);
}

static void *
xapp_iram_to_dram(void *iram)
{
	return (void *)((uintptr_t)iram - XAPP_IRAM_DRAM_OFFSET);
}
#endif /* CONFIG_SOC_SERIES_ESP32S3 */

#if WASM_ENABLE_AOT != 0
#ifdef CONFIG_ARM_MPU
/**
 * This function will allow execute from sram region.
 * This is needed for AOT code because by default all soc will
 * disable the execute from SRAM.
 */
static void
disable_mpu_rasr_xn(void)
{
    uint32 index;
    /* Kept the max index as 8 (irrespective of soc) because the sram
       would most likely be set at index 2. */
    for (index = 0U; index < 8; index++) {
        MPU->RNR = index;
#ifdef MPU_RASR_XN_Msk
        if (MPU->RASR & MPU_RASR_XN_Msk) {
            MPU->RASR |= ~MPU_RASR_XN_Msk;
        }
#endif
    }
}
#endif /* end of CONFIG_ARM_MPU */
#endif

#ifndef CONFIG_USERSPACE
static int
_stdout_hook_iwasm(int c)
{
    printk("%c", (char)c);
    return 1;
}
#endif

int
os_thread_sys_init();

void
os_thread_sys_destroy();

int
bh_platform_init()
{
#ifndef CONFIG_USERSPACE
    extern void __stdout_hook_install(int (*hook)(int));
    /* Enable printf() in Zephyr */
    __stdout_hook_install(_stdout_hook_iwasm);
#endif

#if WASM_ENABLE_AOT != 0
#ifdef CONFIG_ARM_MPU
    /* Enable executable memory support */
    disable_mpu_rasr_xn();
#endif
#endif

    return os_thread_sys_init();
}

void
bh_platform_destroy()
{
    os_thread_sys_destroy();
}

void *
os_malloc(unsigned size)
{
    return malloc(size);
}

void *
os_realloc(void *ptr, unsigned size)
{
    return realloc(ptr, size);
}

void
os_free(void *ptr)
{
    free(ptr);
}

int
os_dumps_proc_mem_info(char *out, unsigned int size)
{
    return -1;
}

#if 0
struct out_context {
    int count;
};

typedef int (*out_func_t)(int c, void *ctx);

static int
char_out(int c, void *ctx)
{
    struct out_context *out_ctx = (struct out_context*)ctx;
    out_ctx->count++;
    return _stdout_hook_iwasm(c);
}

int
os_vprintf(const char *fmt, va_list ap)
{
#if 0
    struct out_context ctx = { 0 };
    cbvprintf(char_out, &ctx, fmt, ap);
    return ctx.count;
#else
    vprintk(fmt, ap);
    return 0;
#endif
}
#endif

int
os_printf(const char *format, ...)
{
    int ret = 0;
    va_list ap;

    va_start(ap, format);
#ifndef BH_VPRINTF
    ret += vprintf(format, ap);
#else
    ret += BH_VPRINTF(format, ap);
#endif
    va_end(ap);

    return ret;
}

int
os_vprintf(const char *format, va_list ap)
{
#ifndef BH_VPRINTF
    return vprintf(format, ap);
#else
    return BH_VPRINTF(format, ap);
#endif
}

#if KERNEL_VERSION_NUMBER <= 0x020400 /* version 2.4.0 */
void
abort(void)
{
    int i = 0;
    os_printf("%d\n", 1 / i);
}
#endif

#if KERNEL_VERSION_NUMBER <= 0x010E01 /* version 1.14.1 */
size_t
strspn(const char *s, const char *accept)
{
    os_printf("## unimplemented function %s called", __FUNCTION__);
    return 0;
}

size_t
strcspn(const char *s, const char *reject)
{
    os_printf("## unimplemented function %s called", __FUNCTION__);
    return 0;
}
#endif

void *
os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file)
{
	void *addr = NULL;

	(void)hint;
	(void)flags;
	(void)file;

	if ((uint64)size >= UINT32_MAX) {
		return NULL;
	}

	if (prot & MMAP_PROT_EXEC) {
		if (exec_mem_alloc_func) {
			addr = exec_mem_alloc_func((uint32)size);
			if (addr) {
				memset(addr, 0, size);
			}
			return addr;
		}
#if defined(XAPP_AOT_PSRAM)
		{
			/* Data-bus pointer in, instruction-bus pointer out.
			 * 0x3Cxxxxxx and 0x420xxxxx are one PSRAM page. */
			void *dram = shared_multi_heap_aligned_alloc(
				SMH_REG_ATTR_EXTERNAL, 32, size);

			if (dram) {
				void *exec;

				memset(dram, 0, size);
				exec = (void *)((uintptr_t)dram +
						(SOC_IROM_LOW - SOC_DROM_LOW));
				printk("aot exec: psram write=%p exec=%p size=%u\n",
				       dram, exec, (unsigned)size);
				return exec;
			}
		}
#endif
#if defined(CONFIG_SOC_SERIES_ESP32S3)
		{
			/*
			 * PSRAM alloc failed. Internal DRAM, IRAM alias, so
			 * the CPU can still fetch the instructions.
			 */
			void *dram = k_aligned_alloc(8, size);

			if (!dram) {
				return NULL;
			}
			memset(dram, 0, size);
			return xapp_dram_to_iram(dram);
		}
#else
		addr = BH_MALLOC(size);
		if (addr) {
			memset(addr, 0, size);
		}
		return addr;
#endif
	}

	addr = BH_MALLOC(size);
	if (addr) {
		memset(addr, 0, size);
	}
	return addr;
}

void *
os_mremap(void *old_addr, size_t old_size, size_t new_size)
{
	return os_mremap_slow(old_addr, old_size, new_size);
}

void
os_munmap(void *addr, size_t size)
{
	(void)size;

	if (!addr) {
		return;
	}

#if defined(XAPP_AOT_PSRAM)
	{
		uintptr_t a = (uintptr_t)addr;

		if (a >= SOC_IROM_LOW && a < SOC_IROM_HIGH) {
			shared_multi_heap_free((void *)(a - (SOC_IROM_LOW - SOC_DROM_LOW)));
			return;
		}
	}
#endif
#if defined(CONFIG_SOC_SERIES_ESP32S3)
	if (xapp_addr_in_iram(addr)) {
		k_free(xapp_iram_to_dram(addr));
		return;
	}
#endif
	if (exec_mem_free_func) {
		exec_mem_free_func(addr);
	} else {
		BH_FREE(addr);
	}
}

int
os_mprotect(void *addr, size_t size, int prot)
{
	(void)addr;
	(void)size;
	(void)prot;
	return 0;
}

#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
void *
os_get_dbus_mirror(void *ibus)
{
#if defined(XAPP_AOT_PSRAM)
	{
		uintptr_t a = (uintptr_t)ibus;

		if (a >= SOC_IROM_LOW && a < SOC_IROM_HIGH) {
			return (void *)(a - (SOC_IROM_LOW - SOC_DROM_LOW));
		}
	}
#endif
#if defined(CONFIG_SOC_SERIES_ESP32S3)
	if (xapp_addr_in_iram(ibus)) {
		return xapp_iram_to_dram(ibus);
	}
#endif
	return ibus;
}
#endif

void
os_dcache_flush(void)
{
#if defined(CONFIG_CPU_CORTEX_M7) && defined(CONFIG_ARM_MPU)
#if KERNEL_VERSION_NUMBER < 0x030300 /* version 3.3.0 */
	uint32 key;
	key = irq_lock();
	SCB_CleanDCache();
	irq_unlock(key);
#else
	sys_cache_data_flush_all();
#endif
#elif defined(CONFIG_SOC_CVF_EM7D) && defined(CONFIG_ARC_MPU) \
	&& defined(CONFIG_CACHE_FLUSHING)
	__asm__ __volatile__("sync");
	z_arc_v2_aux_reg_write(_ARC_V2_DC_FLSH, BIT(0));
	__asm__ __volatile__("sync");
#elif defined(CONFIG_SOC_SERIES_ESP32S3)
	sys_cache_data_flush_all();
#endif
}

void
os_icache_flush(void *start, size_t len)
{
#if defined(XAPP_AOT_PSRAM)
	/* M2C rejects ESP_CACHE_MSYNC_FLAG_UNALIGNED, so the instruction
	 * range is expanded to whole lines. The data writeback may stay
	 * unaligned. One core is running; the other stays stalled, so
	 * this does not also freeze a second instruction stream. */
	const size_t line = CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE;
	uintptr_t ibus = (uintptr_t)start;
	uintptr_t dbus = ibus;
	uintptr_t first, last;

	if (line != 0u && len != 0u &&
	    ibus >= SOC_IROM_LOW && ibus < SOC_IROM_HIGH) {
		dbus = ibus - (SOC_IROM_LOW - SOC_DROM_LOW);
		(void)esp_cache_msync((void *)dbus, len,
				      ESP_CACHE_MSYNC_FLAG_DIR_C2M |
				      ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
				      ESP_CACHE_MSYNC_FLAG_UNALIGNED);
		first = ibus & ~(uintptr_t)(line - 1u);
		last = (ibus + len + line - 1u) & ~(uintptr_t)(line - 1u);
		(void)esp_cache_msync((void *)first, last - first,
				      ESP_CACHE_MSYNC_FLAG_DIR_M2C |
				      ESP_CACHE_MSYNC_FLAG_TYPE_INST);
		return;
	}
#endif
#if KERNEL_VERSION_NUMBER >= 0x030300 /* version 3.3.0 */
#if defined(CONFIG_SOC_SERIES_ESP32S3)
	ARG_UNUSED(start);
	ARG_UNUSED(len);
	sys_cache_instr_invd_all();
#else
	sys_cache_instr_flush_range(start, len);
#endif
#else
	ARG_UNUSED(start);
	ARG_UNUSED(len);
#endif
}

void
set_exec_mem_alloc_func(exec_mem_alloc_func_t alloc_func,
                        exec_mem_free_func_t free_func)
{
    exec_mem_alloc_func = alloc_func;
    exec_mem_free_func = free_func;
}

os_raw_file_handle
os_invalid_raw_handle(void)
{
    return -1;
}
