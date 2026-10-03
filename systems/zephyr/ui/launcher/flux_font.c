/* SPDX-License-Identifier: MIT */
#include "flux_font.h"
#include <string.h>
typedef struct {
 uint32_t cp,offset;
 uint8_t advance,w,h;
 int8_t left,y;
} flux_glyph_t;
typedef struct {
 const flux_glyph_t *glyphs;
 const uint8_t *bitmap;
 unsigned count;
} flux_face_t;
static int glyph_index(const flux_face_t *face,uint32_t cp)
{
 unsigned lo=0,hi=face->count;
 while(lo<hi) {unsigned mid=(lo+hi)/2;
  if(face->glyphs[mid].cp<cp) lo=mid+1;else hi=mid;
 }
 return lo<face->count && face->glyphs[lo].cp==cp?(int)lo:-1;
}
static bool flux_glyph_dsc(const lv_font_t *font,lv_font_glyph_dsc_t *d,uint32_t cp,uint32_t next)
{
 (void)next;
 const flux_face_t *face=font->dsc;int index=glyph_index(face,cp);
 if(index<0) return false;
 const flux_glyph_t *g=&face->glyphs[index];
 d->adv_w=g->advance;d->box_w=g->w;d->box_h=g->h;
 d->ofs_x=g->left;d->ofs_y=g->y;d->stride=g->w;
 d->format=LV_FONT_GLYPH_FORMAT_A8;d->is_placeholder=false;
 d->gid.index=index;d->entry=NULL;return true;
}
static const void *flux_glyph_bitmap(lv_font_glyph_dsc_t *d,lv_draw_buf_t *buffer)
{
 const flux_face_t *face=d->resolved_font->dsc;
 const flux_glyph_t *g=&face->glyphs[d->gid.index];
 const uint8_t *bitmap=face->bitmap+g->offset;
 if(!g->w || !g->h) return NULL;
 if(d->req_raw_bitmap) return bitmap;
 if(buffer) {
  memcpy(buffer->data,bitmap,g->w*g->h);
  return buffer;
 }
 static lv_draw_buf_t wrapper;
 lv_draw_buf_init(&wrapper,g->w,g->h,LV_COLOR_FORMAT_A8,g->w,
                  (void *)bitmap,g->w*g->h);
 return &wrapper;
}
#include "flux_fonts.inc"
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
 const lv_font_t *font=NULL;
 for(unsigned i=0;i<sizeof(fonts)/sizeof(fonts[0]);i++) if(fonts[i].px==pixels) {font=fonts[i].font;break;}
 if(!font) return NULL;
 /* Unknown installed names retain the SD font fallback. */
 const unsigned char *p=(const unsigned char *)text;
 while(*p) {
  uint32_t cp=*p++;unsigned continuation=0;
  if(cp>=0xf0) {cp&=7;continuation=3;}
  else if(cp>=0xe0) {cp&=15;continuation=2;}
  else if(cp>=0xc0) {cp&=31;continuation=1;}
  while(continuation--) {if((*p&0xc0)!=0x80) return NULL;cp=(cp<<6)|(*p++&63);}
  if(cp!='\n' && cp!='\r' && glyph_index(font->dsc,cp)<0) return NULL;
 }
 return font;
}
