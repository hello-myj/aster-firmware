/* SPDX-License-Identifier: MIT */
#include "aster_platform.h"
#include "aster_idf_runtime.h"
#include "graphics.h"
#include "aster_mem.h"
#include "driver/ledc.h"
#include "sdkconfig.h"
#include <string.h>
int aster_plat_display_count(void) {return aster_idf_display_count();}
int aster_plat_display_orientation_get(void) {return ASTER_ORIENT_PORTRAIT;}
int aster_plat_display_orientation_set(int orientation)
{return orientation==ASTER_ORIENT_PORTRAIT?0:ASTER_ENOTSUP;}
int aster_plat_display_write_at_ex(const uint8_t *data,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t pitch,bool swapped)
{
    if(!swapped)return aster_plat_display_write_at(data,x,y,w,h,pitch);
    if(!data || !w || !h || w>480 || h>480 || pitch<w)return ASTER_EINVAL;
    uint16_t line[480];
    for(uint32_t row=0;row<h;row++) {
        const uint8_t *p=data+(size_t)row*pitch*2;
        for(uint32_t col=0;col<w;col++)line[col]=((uint16_t)p[col*2]<<8)|p[col*2+1];
        int r=aster_plat_display_write_at((uint8_t *)line,x,y+row,w,1,w);if(r)return r;
    }
    return 0;
}
bool aster_plat_display_glass_frame(const uint8_t **data,uint32_t *w,uint32_t *h,uint32_t *pitch)
{
    static uint8_t *copy;
    const uint8_t *pixels=aster_idf_capture();if(!pixels)return false;
    if(!copy)copy=aster_psram_alloc(480*480*2);
    if(!copy)return false;
    for(unsigned i=0;i<480*480*2;i+=2){copy[i]=pixels[i+1];copy[i+1]=pixels[i];}
    *data=copy;*w=*h=*pitch=480;return true;
}
int aster_plat_input_poll(aster_touch_evt_t *out,int capacity)
{
    if(!capacity)return 0;
    aster_event_t event;if(!aster_idf_touch_event(&event))return 0;
    *out=(aster_touch_evt_t){.x=event.u.touch.x,.y=event.u.touch.y,.action=event.u.touch.action,.slot=0,
        .contact_mask=event.u.touch.action==ASTER_TOUCH_UP?0:1};return 1;
}
int aster_plat_input_keys_poll(aster_key_evt_t *out,int capacity) {(void)out;(void)capacity;return 0;}
int aster_plat_backlight_set(bool on,uint8_t level)
{
    static bool ready;
    esp_err_t r;
    if(!ready) {
        ledc_timer_config_t timer={.speed_mode=LEDC_LOW_SPEED_MODE,.duty_resolution=LEDC_TIMER_8_BIT,
            .timer_num=LEDC_TIMER_1,.freq_hz=5000,.clk_cfg=LEDC_AUTO_CLK};
        r=ledc_timer_config(&timer);if(r!=ESP_OK)return ASTER_EIO;
        ledc_channel_config_t channel={.gpio_num=CONFIG_ASTER_P4_BACKLIGHT_GPIO,.speed_mode=LEDC_LOW_SPEED_MODE,
            .channel=LEDC_CHANNEL_0,.timer_sel=LEDC_TIMER_1,.duty=on?level:0};
        r=ledc_channel_config(&channel);if(r!=ESP_OK)return ASTER_EIO;
        ready=true;
    }
    r=ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,on?level:0);
    if(r==ESP_OK)r=ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0);
    return r==ESP_OK?0:ASTER_EIO;
}
