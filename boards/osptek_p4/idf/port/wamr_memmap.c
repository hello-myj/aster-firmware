/* SPDX-License-Identifier: MIT */
#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#include "esp_heap_caps.h"
#include "esp_ipc.h"
#include "esp_cpu.h"
#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"
#include "esp_mmu_map.h"
#include "esp_memory_utils.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#if !CONFIG_IDF_TARGET_ESP32P4
#error "This executable memory adapter is specific to ESP32-P4"
#endif
void *os_mmap(void *hint,size_t size,int prot,int flags,os_file_handle file)
{
    (void)hint;(void)flags;(void)file;
    if(!size)return NULL;
    /* P4's PSRAM region has both instruction and data buses. Confirm the MMU
     * instruction address instead of treating heap capability flags as proof
     * of executability. Other SoCs retain their vendor adapter. */
    uint32_t caps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
    size_t alignment=16;
    if(prot&MMAP_PROT_EXEC) {
#if CONFIG_WAMR_ENABLE_AOT
        if(esp_cache_get_alignment(caps,&alignment)!=ESP_OK || !alignment ||
           (alignment&(alignment-1)) || size>SIZE_MAX-(alignment-1))return NULL;
        size=(size+alignment-1)&~(alignment-1);
#else
        return NULL;
#endif
    }
    void *p=heap_caps_aligned_alloc(alignment,size,caps);
    if(p && (prot&MMAP_PROT_EXEC)) {
        esp_paddr_t first,last;mmu_target_t first_target,last_target;void *instruction=NULL;
        if(esp_mmu_vaddr_to_paddr(p,&first,&first_target)!=ESP_OK ||
           esp_mmu_vaddr_to_paddr((uint8_t*)p+size-1,&last,&last_target)!=ESP_OK ||
           first_target!=MMU_TARGET_PSRAM0 || last_target!=first_target || last-first!=size-1 ||
           esp_mmu_paddr_to_vaddr(first,first_target,MMU_VADDR_INSTRUCTION,&instruction)!=ESP_OK || instruction!=p) {
            printf("AOT code: PSRAM instruction mapping unavailable\n");
            heap_caps_free(p);return NULL;
        }
        printf("AOT code: PSRAM bytes=%u address=%p\n",(unsigned)size,p);
    }
    if(p)memset(p,0,size);
    return p;
}
void *os_mremap(void *old,size_t before,size_t after) {return os_mremap_slow(old,before,after);}
void os_munmap(void *p,size_t size) {(void)size;heap_caps_free(p);}
int os_mprotect(void *p,size_t size,int prot) {(void)p;(void)size;(void)prot;return 0;}
static void synchronize(void *unused) {(void)unused;__asm__ volatile("fence rw,rw\n\tfence.i":::"memory");}
void os_dcache_flush(void) {__asm__ volatile("fence rw,rw":::"memory");}
void os_icache_flush(void *start,size_t length)
{
    if(start && length && esp_ptr_external_ram(start)) {
        size_t alignment=0;
        ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT,&alignment));
        uintptr_t first=(uintptr_t)start&~(alignment-1);
        uintptr_t end=((uintptr_t)start+length+alignment-1)&~(alignment-1);
        ESP_ERROR_CHECK(esp_cache_msync((void*)first,end-first,ESP_CACHE_MSYNC_FLAG_DIR_C2M|ESP_CACHE_MSYNC_FLAG_TYPE_DATA));
        ESP_ERROR_CHECK(esp_cache_msync((void*)first,end-first,ESP_CACHE_MSYNC_FLAG_DIR_M2C|ESP_CACHE_MSYNC_FLAG_TYPE_INST));
    }
    synchronize(NULL);
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    /* The VM task can migrate between harts between load and execution. */
    ESP_ERROR_CHECK(esp_ipc_call_blocking(esp_cpu_get_core_id()^1,synchronize,NULL));
#endif
}
