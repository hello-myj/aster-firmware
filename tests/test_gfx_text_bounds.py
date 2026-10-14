"""Exercise the firmware text rasterizer with ascent, descent and clip sentinels."""
import unittest
from test_gfx_resources import PREFIX
from test_waveshare import ROOT, run_c

class TextBounds(unittest.TestCase):
 def test_glyph_box_publication_and_explicit_clip(self):
  source=(ROOT/'systems/zephyr/ui/ui_gfx.c').read_text()
  clip=source[source.index('static inline bool clip_rect('):source.index('/* Every op that names a point')]
  op=source[source.index('static void op_text('):source.index('/* ---------------- sprite cache')]
  prefix=PREFIX.replace('GFX_HEADER',str(ROOT/'contract/include/aster/gfx.h')).replace('ERROR_HEADER',str(ROOT/'contract/include/aster/error.h'))
  prefix=prefix.replace('static uint16_t out_blend(uint16_t d,uint16_t c,uint32_t a){(void)d;blends++;return c^(uint16_t)a;}',
      '#include \"'+str(ROOT/'systems/zephyr/core/aster_color565.h')+'\"\nstatic uint16_t out_blend(uint16_t d,uint16_t c,uint32_t a){blends++;return out_pack(blend565(out_pack(d),c,a));}')
  run_c(prefix+r'''
#define ASTER_FONT_UI_PX 16
struct font;
typedef struct {uint32_t adv_w,box_w,box_h;int ofs_x,ofs_y;const struct font *resolved_font;} lv_font_glyph_dsc_t;
typedef struct font {int line_height,base_line;void (*release_glyph)(const struct font*,lv_font_glyph_dsc_t*);} lv_font_t;
typedef struct {struct {uint32_t w,h,stride;} header;uint8_t *data;} lv_draw_buf_t;
static lv_font_t font={21,4,0};
static uint8_t mask[16]={255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255};
static lv_draw_buf_t db={{2,8,2},mask};
static const lv_font_t *aster_ui_font(uint8_t px){(void)px;return &font;}
static uint32_t utf8_next_t(const uint8_t **p){return *(*p)++;}
static bool lv_font_get_glyph_dsc(const lv_font_t *f,lv_font_glyph_dsc_t *g,uint32_t cp,uint32_t next){(void)cp;(void)next;*g=(lv_font_glyph_dsc_t){3,2,8,0,2,f};return true;}
static void *lv_font_get_glyph_bitmap(lv_font_glyph_dsc_t *g,void *p){(void)g;(void)p;return &db;}
#define LV_COLOR_FORMAT_A8 0
static uint32_t lv_draw_buf_width_to_stride(uint32_t w,int fmt){(void)fmt;return w;}
'''+clip+op+r'''
int main(void){
 (void)live;(void)fail_alloc;(void)fail_decode;(void)aster_psram_alloc;(void)aster_psram_free;
 (void)aster_imgdec_probe;(void)aster_imgdec_render;
 uint16_t pixels[20*8];for(int i=0;i<160;i++)pixels[i]=0xabcd;
 ppg_target_t t={pixels,6,20,8};acc_t a={0};uint8_t arena[]="A";
 aster_gfx_cmd_t c={{ASTER_GFX_TEXT,ASTER_GFX_PACK(1,12),ASTER_GFX_PACK(16,0xffff),0,1,0}};
 op_text(&t,&c,arena,1,&a);
 assert(a.has&&a.x0==1&&a.x1==3&&a.y0==2&&a.y1==10&&blends==16);
 for(int y=0;y<20;y++)for(int x=0;x<8;x++)assert((pixels[y*8+x]!=0xabcd)==(x>=1&&x<3&&y>=2&&y<10));
 for(int i=0;i<160;i++)pixels[i]=0xabcd;
 s_clip_x=2;s_clip_y=4;s_clip_w=1;s_clip_h=2;blends=0;a.has=false;
 op_text(&t,&c,arena,1,&a);
 assert(a.has&&a.x0==2&&a.x1==3&&a.y0==4&&a.y1==6&&blends==2);
 for(int y=0;y<20;y++)for(int x=0;x<8;x++)assert((pixels[y*8+x]!=0xabcd)==(x==2&&y>=4&&y<6));
 s_clip_y=15;blends=0;a.has=false;op_text(&t,&c,arena,1,&a);assert(!a.has&&blends==0);
 s_clip_x=s_clip_y=s_clip_w=s_clip_h=0;
 mask[0]=85;mask[1]=170;mask[2]=0;mask[3]=255;
 c.w[5]=0x10000|0x1234;
 for(int i=0;i<160;i++)pixels[i]=0xf800;
 op_text(&t,&c,arena,1,&a);
 assert(pixels[2*8+1]==blend565(0x1234,0xffff,85));
 assert(pixels[2*8+2]==blend565(0x1234,0xffff,170));
 assert(pixels[3*8+1]==0x1234&&pixels[3*8+2]==0xffff);
 uint16_t once=pixels[2*8+1];op_text(&t,&c,arena,1,&a);assert(pixels[2*8+1]==once);
 return 0;}
''')
