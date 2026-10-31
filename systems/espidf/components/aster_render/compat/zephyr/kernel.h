/* SPDX-License-Identifier: MIT */
#pragma once
#include "zephyr/sys/util.h"
#include "esp_timer.h"
static inline uint32_t k_cycle_get_32(void) { return (uint32_t)esp_timer_get_time(); }
static inline uint32_t k_uptime_get_32(void) { return (uint32_t)(esp_timer_get_time()/1000); }
static inline uint64_t k_cyc_to_us_floor64(uint32_t t) { return t; }
static inline uint32_t k_cyc_to_us_floor32(uint32_t t) { return t; }

#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "aster_mem.h"
#define K_FOREVER portMAX_DELAY
#define K_NO_WAIT 0
#define K_MSEC(ms) pdMS_TO_TICKS(ms)
#define k_malloc aster_psram_alloc
#define k_free aster_psram_free
struct k_mutex { StaticSemaphore_t storage; SemaphoreHandle_t handle; portMUX_TYPE guard; };
#define K_MUTEX_DEFINE(name) struct k_mutex name={.handle=NULL,.guard=portMUX_INITIALIZER_UNLOCKED}
static inline int k_mutex_lock(struct k_mutex *m,TickType_t timeout)
{
    portENTER_CRITICAL(&m->guard);
    if(!m->handle)m->handle=xSemaphoreCreateRecursiveMutexStatic(&m->storage);
    portEXIT_CRITICAL(&m->guard);
    return xSemaphoreTakeRecursive(m->handle,timeout)==pdTRUE ? 0 : -EBUSY;
}
static inline void k_mutex_unlock(struct k_mutex *m) { xSemaphoreGiveRecursive(m->handle); }
static inline int64_t k_uptime_get(void) { return esp_timer_get_time()/1000; }
static inline void k_msleep(unsigned ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
#define k_sleep(ticks) vTaskDelay(ticks)
#ifndef ASTER_IDF_HOST_TEST
#include <stdatomic.h>
#include "freertos/queue.h"
struct k_sem {StaticSemaphore_t storage;SemaphoreHandle_t handle;unsigned initial,limit;portMUX_TYPE guard;};
#define K_SEM_DEFINE(n,a,b) struct k_sem n={.initial=a,.limit=b,.guard=portMUX_INITIALIZER_UNLOCKED}
static inline SemaphoreHandle_t aster_sem(struct k_sem *s)
{
    portENTER_CRITICAL(&s->guard);
    if(!s->handle)s->handle=xSemaphoreCreateCountingStatic(s->limit,s->initial,&s->storage);
    portEXIT_CRITICAL(&s->guard);return s->handle;
}
static inline int k_sem_take(struct k_sem *s,TickType_t t) {return xSemaphoreTake(aster_sem(s),t)==pdTRUE?0:-EAGAIN;}
static inline void k_sem_give(struct k_sem *s) {xSemaphoreGive(aster_sem(s));}
static inline void k_sem_reset(struct k_sem *s) {while(xSemaphoreTake(aster_sem(s),0)==pdTRUE){}}
typedef StackType_t k_thread_stack_t;
struct k_thread {StaticTask_t storage;TaskHandle_t handle;void (*entry)(void *,void *,void *);void *a,*b,*c;atomic_bool finished;StaticSemaphore_t start_storage;SemaphoreHandle_t start;};
#define K_KERNEL_STACK_LEN(n) (n)
#define K_KERNEL_STACK_RESERVED 0
#define K_FP_REGS 0
#define K_PRIO_PREEMPT(n) (n)
static inline void aster_thread_entry(void *p)
{struct k_thread *t=p;xSemaphoreTake(t->start,portMAX_DELAY);t->entry(t->a,t->b,t->c);atomic_store(&t->finished,true);vTaskSuspend(NULL);}
static inline struct k_thread *k_thread_create(struct k_thread *t,k_thread_stack_t *stack,size_t bytes,
    void (*entry)(void *,void *,void *),void *a,void *b,void *c,int priority,int options,TickType_t delay)
{
    (void)options;(void)delay;t->entry=entry;t->a=a;t->b=b;t->c=c;atomic_store(&t->finished,false);
    t->start=xSemaphoreCreateBinaryStatic(&t->start_storage);
    if(delay!=K_FOREVER)xSemaphoreGive(t->start);
    t->handle=xTaskCreateStatic(aster_thread_entry,"aster_worker",bytes,t,priority,stack,&t->storage);
    /* Static task construction has no dynamic allocation; failure is fatal. */
    configASSERT(t->handle);return t;
}
static inline void k_thread_name_set(struct k_thread *t,const char *name) {(void)t;(void)name;}
static inline void k_thread_start(struct k_thread *t) {xSemaphoreGive(t->start);}
static inline int k_thread_join(struct k_thread *t,TickType_t timeout)
{
    TickType_t begin=xTaskGetTickCount();
    while(t->handle) {
        if(atomic_load(&t->finished) && eTaskGetState(t->handle)==eSuspended) {
            vTaskDelete(t->handle);t->handle=NULL;return 0;
        }
        if(timeout!=portMAX_DELAY && xTaskGetTickCount()-begin>=timeout)return -EAGAIN;
        vTaskDelay(1);
    }
    return 0;
}
struct k_mem_slab {StaticQueue_t storage;QueueHandle_t queue;uint8_t *buffer,*queue_buffer;size_t block,count;portMUX_TYPE guard;};
#define K_MEM_SLAB_DEFINE_STATIC(n,b,c,a) \
    static uint8_t n##_bytes[(b)*(c)] __attribute__((aligned(a))); \
    static uint8_t n##_queue[(c)*sizeof(void *)]; \
    static struct k_mem_slab n={.buffer=n##_bytes,.queue_buffer=n##_queue,.block=b,.count=c,.guard=portMUX_INITIALIZER_UNLOCKED}
static inline QueueHandle_t aster_slab(struct k_mem_slab *s)
{
    portENTER_CRITICAL(&s->guard);
    if(!s->queue) {
        s->queue=xQueueCreateStatic(s->count,sizeof(void *),s->queue_buffer,&s->storage);
        for(size_t i=0;i<s->count;i++){void *p=s->buffer+i*s->block;xQueueSend(s->queue,&p,0);}
    }
    portEXIT_CRITICAL(&s->guard);return s->queue;
}
static inline int k_mem_slab_alloc(struct k_mem_slab *s,void **p,TickType_t timeout)
{return xQueueReceive(aster_slab(s),p,timeout)==pdTRUE?0:-EAGAIN;}
static inline void k_mem_slab_free(struct k_mem_slab *s,void *p)
{configASSERT(xQueueSend(aster_slab(s),&p,0)==pdTRUE);}
#endif
