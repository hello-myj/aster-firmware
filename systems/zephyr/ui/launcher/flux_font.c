/* SPDX-License-Identifier: MIT */
#include "flux_font.h"
#include "../ui_font.h"
unsigned aster_flux_font_pixels(unsigned size,unsigned zoom,const char *text)
{
 unsigned pixels=(size*zoom+128)/256;
 /* CJK strokes cannot stay legible at the reference's 8-10 px scale.
  * Chrome labels retain native integral ppem and never shrink these faces. */
 unsigned minimum=12;
 for(const unsigned char *p=(const unsigned char *)text;*p;p++)
  if(*p>=0x80) {minimum=14;break;}
 return pixels<minimum?minimum:pixels;
}
const lv_font_t *aster_flux_font(unsigned pixels,const char *text)
{
 const lv_font_t *font=aster_ui_font((uint8_t)pixels);
 aster_ui_font_warm((uint8_t)pixels,text);
 return font;
}
