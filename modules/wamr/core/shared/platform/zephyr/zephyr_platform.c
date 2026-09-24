/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-FileCopyrightText: 2024 Siemens AG (For Zephyr usermode changes)
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"

#include <zephyr/kernel.h>
#include <zephyr/cache.h>

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
#if defined(CONFIG_SOC_SERIES_ESP32S3)
		/*
		 * Allocate from internal DRAM (kernel heap), return IRAM
		 * alias so the CPU can fetch AOT instructions.
		 */
		void *dram = k_aligned_alloc(8, size);

		if (!dram) {
			return NULL;
		}
		memset(dram, 0, size);
		return xapp_dram_to_iram(dram);
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
