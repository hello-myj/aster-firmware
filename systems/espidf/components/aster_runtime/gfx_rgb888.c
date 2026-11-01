/* SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <lvgl.h>
#include "gfx_rgb888.h"
#include "ui_font.h"
#include "font_sd.h"
#include "draw/lv_draw_private.h"
static void swap_channels(uint8_t *p,size_t count) {for(size_t i=0;i<count;i++,p+=3){uint8_t c=p[0];p[0]=p[2];p[2]=c;}}
int aster_idf_gfx888(uint8_t *pixels,uint32_t width,uint32_t height,bool rgb,
 const aster_gfx_rgb888_cmd_t *commands,uint32_t count,const uint8_t *arena,uint32_t arena_len)
{
    size_t text_bytes=0;char *texts[256]={0};
    /* Validate the complete immutable batch before touching guest pixels. */
    for(unsigned i=0;i<count;i++) {
        const uint32_t *w=commands[i].w;
        if(w[0]>ASTER_RGB888_TRI || w[7]>255 || (w[3]&0xff000000u))return ASTER_EINVAL;
        if(w[0]==ASTER_RGB888_RECT && (w[4]>65535 || w[6]>1024))return ASTER_EINVAL;
        if(w[0]==ASTER_RGB888_GRAD && ((w[4]&0xff000000u) || w[6]>1))return ASTER_EINVAL;
        if(w[0]==ASTER_RGB888_LINE && (!w[4] || w[4]>1024))return ASTER_EINVAL;
        if(w[0]==ASTER_RGB888_CIRCLE && (w[2]>32767 || w[4]>1024))return ASTER_EINVAL;
        if(w[0]==ASTER_RGB888_TEXT) {
            if(w[2]>255 || (w[4]&0xfe000000u) || w[5]>arena_len || w[6]>arena_len-w[5])return ASTER_EINVAL;
            text_bytes+=(size_t)w[6]+1;if(text_bytes>ASTER_GFX_FRAME_MAX_ARENA+256)return ASTER_EINVAL;
        }
    }
    char *storage=text_bytes?malloc(text_bytes):NULL;
    if(text_bytes && !storage)return ASTER_ENOMEM;
    char *cursor=storage;
    for(unsigned i=0;i<count;i++)if(commands[i].w[0]==ASTER_RGB888_TEXT) {
        texts[i]=cursor;if(commands[i].w[6])memcpy(cursor,arena+commands[i].w[5],commands[i].w[6]);cursor[commands[i].w[6]]=0;cursor+=commands[i].w[6]+1;
    }
    lv_obj_t *canvas=lv_canvas_create(lv_layer_top());
    if(!canvas){free(storage);return ASTER_ENOMEM;}
    lv_obj_add_flag(canvas,LV_OBJ_FLAG_HIDDEN);
    lv_draw_buf_t buffer;
    if(lv_draw_buf_init(&buffer,width,height,LV_COLOR_FORMAT_RGB888,width*3,pixels,width*height*3)!=LV_RESULT_OK) {
        lv_obj_delete(canvas);free(storage);return ASTER_EINVAL;
    }
    if(rgb)swap_channels(pixels,(size_t)width*height);
    lv_canvas_set_draw_buf(canvas,&buffer);
    (void)aster_font_sd_poll();(void)aster_font_sd_commit();aster_font_sd_io_begin();
    for(unsigned i=0;i<count;i++) {
        const uint32_t *w=commands[i].w;
        int x=ASTER_GFX_P16(w[1]),y=ASTER_GFX_P16H(w[1]);
        lv_layer_t layer;lv_canvas_init_layer(canvas,&layer);
        lv_area_t area={x,y,x+ASTER_GFX_U16(w[2])-1,y+ASTER_GFX_HI16(w[2])-1};
        if(w[0]==ASTER_RGB888_LINE) {
            lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);
            d.p1=(lv_point_precise_t){x,y};d.p2=(lv_point_precise_t){ASTER_GFX_P16(w[2]),ASTER_GFX_P16H(w[2])};
            d.color=lv_color_hex(w[3]);d.opa=w[7];d.width=w[4];lv_draw_line(&layer,&d);
        } else if(w[0]==ASTER_RGB888_TRI) {
            lv_draw_triangle_dsc_t d;lv_draw_triangle_dsc_init(&d);
            d.p[0]=(lv_point_precise_t){x,y};d.p[1]=(lv_point_precise_t){ASTER_GFX_P16(w[2]),ASTER_GFX_P16H(w[2])};
            d.p[2]=(lv_point_precise_t){ASTER_GFX_P16(w[4]),ASTER_GFX_P16H(w[4])};
            d.color=lv_color_hex(w[3]);d.opa=w[7];lv_draw_triangle(&layer,&d);
        } else if(w[0]==ASTER_RGB888_TEXT) {
            const lv_font_t *font=aster_ui_font(w[2]);
            lv_draw_label_dsc_t d;lv_draw_label_dsc_init(&d);
            d.font=font;d.text=texts[i];d.color=lv_color_hex(w[3]);d.opa=w[7];
            area=(lv_area_t){x,y-font->line_height+font->base_line,(int)width-1,(int)height-1};
            if(w[4]&0x1000000u) {
                lv_draw_rect_dsc_t bg;lv_draw_rect_dsc_init(&bg);bg.bg_color=lv_color_hex(w[4]&0xffffffu);bg.bg_opa=w[7];
                lv_point_t size;lv_text_get_size(&size,texts[i],font,0,0,width,LV_TEXT_FLAG_NONE);
                lv_area_t a={area.x1,area.y1,area.x1+size.x-1,area.y1+size.y-1};lv_draw_rect(&layer,&bg,&a);
            }
            lv_draw_label(&layer,&d,&area);
        } else {
            lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.bg_color=lv_color_hex(w[3]);d.bg_opa=w[7];
            if(w[0]==ASTER_RGB888_CLEAR)area=(lv_area_t){0,0,(int)width-1,(int)height-1};
            if(w[0]==ASTER_RGB888_RECT) {
                d.radius=w[4];if(w[6]){d.bg_opa=0;d.border_color=d.bg_color;d.border_width=w[6];d.border_opa=w[7];}
            }
            if(w[0]==ASTER_RGB888_CIRCLE) {
                area=(lv_area_t){x-(int)w[2],y-(int)w[2],x+(int)w[2],y+(int)w[2]};d.radius=LV_RADIUS_CIRCLE;
                if(w[4]){d.bg_opa=0;d.border_color=d.bg_color;d.border_width=w[4];d.border_opa=w[7];}
            }
            if(w[0]==ASTER_RGB888_GRAD) {
                d.bg_grad.dir=w[6]?LV_GRAD_DIR_HOR:LV_GRAD_DIR_VER;d.bg_grad.stops_count=2;
                d.bg_grad.stops[0].color=lv_color_hex(w[3]);d.bg_grad.stops[0].opa=255;d.bg_grad.stops[0].frac=0;
                d.bg_grad.stops[1].color=lv_color_hex(w[4]);d.bg_grad.stops[1].opa=255;d.bg_grad.stops[1].frac=255;
            }
            if(area.x2>=area.x1 && area.y2>=area.y1)lv_draw_rect(&layer,&d,&area);
        }
        lv_canvas_finish_layer(canvas,&layer);
    }
    aster_font_sd_io_end();lv_obj_delete(canvas);
    if(rgb)swap_channels(pixels,(size_t)width*height);
    free(storage);return count;
}
