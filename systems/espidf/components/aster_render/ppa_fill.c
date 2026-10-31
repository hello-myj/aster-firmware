/* SPDX-License-Identifier: MIT */
/* P4 acceleration for large opaque rectangular fills. Other tasks continue
 * through LVGL SW, including FLUX's arbitrary-angle fused image transform.
 * The IDF PPA driver owns writeback/invalidate of the affected cache lines. */
#include <lvgl.h>
#include "draw/lv_draw_private.h"
#include "misc/lv_area_private.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ppa_fill.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#define PPA_UNIT 81
static ppa_client_handle_t client;
static bool failed;
static uint32_t jobs,errors;
static uint64_t pixels;
static esp_err_t fill(lv_draw_buf_t *buf,const lv_area_t *area,lv_color_t color)
{
    ppa_fill_oper_config_t config={
        .out={.buffer=buf->data,.buffer_size=buf->data_size,
              .pic_w=buf->header.stride/2,.pic_h=buf->header.h,
              .block_offset_x=area->x1,.block_offset_y=area->y1,.fill_cm=PPA_FILL_COLOR_MODE_RGB565},
        .fill_block_w=lv_area_get_width(area),.fill_block_h=lv_area_get_height(area),
        .fill_argb_color={.val=lv_color_to_u32(color)},.mode=PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_fill(client,&config);
}
static int32_t evaluate(lv_draw_unit_t *unit,lv_draw_task_t *task)
{
    (void)unit;
    if(failed || task->type!=LV_DRAW_TASK_TYPE_FILL)return 0;
    const lv_draw_fill_dsc_t *d=task->draw_dsc;
    lv_draw_buf_t *buf=task->target_layer->draw_buf;
    if(!buf || task->target_layer->color_format!=LV_COLOR_FORMAT_RGB565 ||
       d->opa!=LV_OPA_COVER || d->radius || d->grad.dir!=LV_GRAD_DIR_NONE ||
       task->opa!=LV_OPA_COVER || ((uintptr_t)buf->data&63) || (buf->data_size&63))return 0;
    lv_area_t area;
    if(!lv_area_intersect(&area,&task->area,&task->clip_area) ||
       !lv_area_intersect(&area,&area,&task->target_layer->buf_area) ||
       (uint32_t)lv_area_get_width(&area)*lv_area_get_height(&area)<4096)return 0;
    if(task->preference_score>70){task->preference_score=70;task->preferred_draw_unit_id=PPA_UNIT;}
    return 1;
}
static int32_t dispatch(lv_draw_unit_t *unit,lv_layer_t *layer)
{
    lv_draw_task_t *task=lv_draw_get_available_task(layer,NULL,PPA_UNIT);
    if(!task || task->preferred_draw_unit_id!=PPA_UNIT)return LV_DRAW_UNIT_IDLE;
    task->state=LV_DRAW_TASK_STATE_IN_PROGRESS;task->draw_unit=unit;
    const lv_draw_fill_dsc_t *d=task->draw_dsc;lv_area_t area;
    if(lv_area_intersect(&area,&task->area,&task->clip_area) &&
       lv_area_intersect(&area,&area,&layer->buf_area)) {
        lv_area_move(&area,-layer->buf_area.x1,-layer->buf_area.y1);
        if(fill(layer->draw_buf,&area,d->color)==ESP_OK){jobs++;pixels+=(uint64_t)lv_area_get_width(&area)*lv_area_get_height(&area);}
        else {
            /* Preserve the frame if the peripheral cannot accept a task. */
            errors++;failed=true;uint16_t color=lv_color_to_u16(d->color);
            for(int y=area.y1;y<=area.y2;y++) {
                uint16_t *row=(uint16_t *)(layer->draw_buf->data+y*layer->draw_buf->header.stride);
                for(int x=area.x1;x<=area.x2;x++)row[x]=color;
            }
        }
    }
    task->state=LV_DRAW_TASK_STATE_FINISHED;lv_draw_dispatch_request();return 1;
}
static bool self_test(void)
{
    uint16_t *data=heap_caps_aligned_alloc(64,32*16*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!data)return false;
    for(unsigned i=0;i<32*16;i++)data[i]=0x07e0;
    lv_draw_buf_t buf;
    bool good=lv_draw_buf_init(&buf,32,16,LV_COLOR_FORMAT_RGB565,64,data,32*16*2)==LV_RESULT_OK;
    lv_area_t area={3,2,19,8};
    if(good)good=fill(&buf,&area,lv_color_hex(0xff0000))==ESP_OK;
    for(unsigned y=0;good && y<16;y++)for(unsigned x=0;x<32;x++)
        if(data[y*32+x]!=((x>=3 && x<=19 && y>=2 && y<=8)?0xf800:0x07e0)){good=false;break;}
    free(data);return good;
}
int aster_idf_ppa_fill_init(void)
{
    if(client)return 0;
    ppa_client_config_t config={.oper_type=PPA_OPERATION_FILL,.max_pending_trans_num=1};
    esp_err_t result=ppa_register_client(&config,&client);
    if(result!=ESP_OK)return result;
    if(!self_test()) {
        ppa_unregister_client(client);client=NULL;
        ESP_LOGW("aster_ppa","RGB565/cache self-test failed; software rendering retained");return ESP_FAIL;
    }
    lv_draw_unit_t *unit=lv_draw_create_unit(sizeof(lv_draw_unit_t));
    unit->name="ASTER_PPA_FILL";unit->evaluate_cb=evaluate;unit->dispatch_cb=dispatch;
    ESP_LOGI("aster_ppa","RGB565/cache self-test passed; large fill acceleration ready");return 0;
}
void aster_idf_ppa_fill_reset(void) {jobs=errors=0;pixels=0;}
void aster_idf_ppa_fill_report(void)
{printf("UI PPA fills=%lu pixels=%llu errors=%lu\n",(unsigned long)jobs,(unsigned long long)pixels,(unsigned long)errors);}
