/* SPDX-License-Identifier: MIT */
#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#include "esp_heap_caps.h"
#include "esp_ipc.h"
#include "esp_cpu.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <string.h>
#if !CONFIG_IDF_TARGET_ESP32P4
#error "This executable memory adapter is specific to ESP32-P4"
#endif
void *os_mmap(void *hint,size_t size,int prot,int flags,os_file_handle file)
{
    (void)hint;(void)flags;(void)file;
    if(!size)return NULL;
    /* Do not assume that arbitrary data-mapped PSRAM is executable. Large AOT
     * images may exhaust internal executable SRAM; the launcher then retries
     * their portable WASM. Physical execution still requires board validation. */
    uint32_t caps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
    if(prot&MMAP_PROT_EXEC) {
#if CONFIG_WAMR_ENABLE_AOT
        /* IDF rejects EXEC|8BIT even on P4's unified byte-accessible SRAM.
         * EXEC selects this internal SRAM; P4 has no inverted IRAM alias. */
        caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_EXEC;
        size_t available=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        size_t reserve=(size_t)CONFIG_ASTER_P4_AOT_INTERNAL_RESERVE_KB*1024;
        if(size>available || reserve>available-size)return NULL;
#else
        return NULL;
#endif
    }
    void *p=heap_caps_aligned_alloc(16,size,caps);
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
    (void)start;(void)length;synchronize(NULL);
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    /* The VM task can migrate between harts between load and execution. */
    ESP_ERROR_CHECK(esp_ipc_call_blocking(esp_cpu_get_core_id()^1,synchronize,NULL));
#endif
}
