"""Compare the real sprite rasterizer with independent scalar nearest sampling."""
import unittest
from test_waveshare import run_c
import test_gfx_resources as resources

REFERENCE=r'''
static void reference(ppg_target_t *t,const aster_gfx_cmd_t *c,acc_t *a){
 uint32_t op=ASTER_GFX_OP(c),scale=(op==ASTER_GFX_BMP_S||op==ASTER_GFX_BMP_SA)?c->w[4]&ASTER_GFX_SCALE_MASK:256;
 uint32_t alpha=op==ASTER_GFX_BMP_A?c->w[4]&255:op==ASTER_GFX_BMP_SA?c->w[5]&255:255;
 sprite_t *sp=sprite_lookup(c->w[2]);if(!sp||scale<8||!alpha)return;
 int32_t x=(int32_t)cmd_xy0(c),y=(int32_t)cmd_xy1(c);
 uint32_t dw=(uint64_t)sp->w*scale/256,dh=(uint64_t)sp->h*scale/256,cx,cy,cw,ch;
 if(!clip_rect(t,x,y,dw,dh,&cx,&cy,&cw,&ch))return;
 acc_merge(a,cx,cy,cx+cw,cy+ch);
 for(uint32_t dy=cy;dy<cy+ch;dy++)for(uint32_t dx=cx;dx<cx+cw;dx++){
  uint32_t sx=(uint64_t)((int64_t)dx-x)*sp->w/dw,sy=(uint64_t)((int64_t)dy-y)*sp->h/dh;
  uint16_t col=sp->buf[(size_t)sy*sp->w+sx];
  if(col) t->px[dy*t->pitch+dx]=alpha==255?out_pack(col):out_blend(t->px[dy*t->pitch+dx],col,alpha);
 }
}
static uint32_t seed=42;
static uint32_t rnd(void){seed=seed*1664525u+1013904223u;return seed>>8;}
'''
class SpriteSampling(unittest.TestCase):
 def test_random_sampling_clips_key_holes_and_byte_order(self):
  run_c(resources.GfxResources().code()+REFERENCE+r'''
int main(void){
 (void)s_last_w;(void)s_last_h;(void)s_last_pitch;(void)s_last_be;
 uint16_t actual[331],expected[331];
 const uint32_t scales[]={0,7,8,17,64,127,128,255,256,257,384,511,512,777,4095};
 const uint32_t alphas[]={0,1,128,254,255};
 const int coords[]={-32768,-1024,-30,-3,0,4,17,32767};
 for(unsigned test=0;test<12000;test++){
  uint32_t w=1+rnd()%160,h=1+rnd()%160,id;
  if(test%40==0){w=1;h=25600;}
  assert(sprite_alloc(w,h,&id)==0);sprite_t *sp=sprite_lookup(id);
  for(uint32_t i=0;i<w*h;i++)sp->buf[i]=(test%3==0||rnd()%4)?(uint16_t)(1+rnd()%65535):0;
  if(test%17==0)memset(sp->buf,0,(size_t)w*h*2);
  sprite_prepare_rows(sp);
  assert(!sp->row_bounds||(size_t)h*4<=SPRITE_ROW_BUDGET);
  for(int i=0;i<331;i++)actual[i]=expected[i]=(uint16_t)rnd();
  ppg_target_t a={actual+16,19,13,23},b={expected+16,19,13,23};acc_t da={0},db={0};
  s_out_be=test&1;s_clip_x=rnd()%22;s_clip_y=rnd()%15;s_clip_w=test%3?rnd()%23:0;s_clip_h=s_clip_w?1+rnd()%15:0;
  aster_gfx_cmd_t c={{ASTER_GFX_BMP_SA,ASTER_GFX_PACK(coords[rnd()%8],coords[rnd()%8]),id,0,scales[rnd()%15],alphas[rnd()%5]}};
  op_bmp(&a,&c,&da);reference(&b,&c,&db);
  assert(!memcmp(actual,expected,sizeof(actual)));
  assert(da.has==db.has);if(da.has)assert(da.x0==db.x0&&da.y0==db.y0&&da.x1==db.x1&&da.y1==db.y1);
  assert(aster_gfx_bmp_release(id)==0&&live==0);
 }
 return 0;
}
''',timeout=15)
 def test_row_cache_allocation_failure_and_lifecycle(self):
  code=resources.GfxResources().code().replace('if(fail_alloc)return NULL;','if(fail_alloc&&live)return NULL;')
  run_c(code+REFERENCE+r'''
int main(void){
 (void)s_last_w;(void)s_last_h;(void)s_last_pitch;(void)s_last_be;
 uint32_t id;assert(sprite_alloc(8,4,&id)==0);sprite_t *sp=sprite_lookup(id);
 for(int i=0;i<32;i++)sp->buf[i]=i%8>=2&&i%8<5?0xf800:0;
 fail_alloc=1;sprite_prepare_rows(sp);assert(!sp->opaque&&!sp->row_bounds&&live==1);
 uint16_t a[64]={0},b[64]={0};ppg_target_t ta={a,8,8,8},tb={b,8,8,8};acc_t aa={0},ab={0};
 aster_gfx_cmd_t cmd={{ASTER_GFX_BMP_SA,ASTER_GFX_PACK(-1,2),id,0,384,128}};
 op_bmp(&ta,&cmd,&aa);reference(&tb,&cmd,&ab);assert(!memcmp(a,b,sizeof(a)));
 fail_alloc=0;sprite_prepare_rows(sp);assert(sp->row_bounds&&live==2);
 for(int y=0;y<4;y++)assert(sp->row_bounds[y*2]==2&&sp->row_bounds[y*2+1]==5);
 memset(a,0,sizeof(a));op_bmp(&ta,&cmd,&aa);assert(!memcmp(a,b,sizeof(a)));
 aster_gfx_drop_all();assert(live==0&&!sprite_lookup(id));
 assert(sprite_alloc(1,25600,&id)==0);sp=sprite_lookup(id);memset(sp->buf,0,51200);sprite_prepare_rows(sp);assert(!sp->row_bounds&&live==1);
 assert(aster_gfx_bmp_release(id)==0&&live==0);return 0;
}
''')
