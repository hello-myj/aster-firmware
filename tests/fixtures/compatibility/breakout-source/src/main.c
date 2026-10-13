#include <aster_imports.h>
#include <aster_gfx_engine.h>
#include "game.h"

static aster_handle_t surface;
static aster_ge_t ge;
static aster_gfx_cmd_t commands[160];
static uint8_t arena[1536];
static game_t game;
static uint32_t last_ms,best;
static int dirty,down_kind,saved_score,panel_canvas;
static uint16_t colors[ROWS];
static int painted,last_x,last_y,last_paddle,last_mode,last_score,last_lives,force_full;
static uint8_t last_bricks[BRICKS];

static void cleanup(void) {if(surface) {aster_surface_destroy(surface);surface=0;}}
static int font_px(int role) {return aster_text_for_display(role,GAME_W,GAME_H);}
static int text(int x,int top,int px,uint16_t color,const char *s) {
 return aster_ge_text(&ge,x,top+aster_text_asc(px),px,color,ASTER_GFX_NOBG,s);
}
static int center(int top,int px,uint16_t color,const char *s) {
 int width=aster_text_measure(px,s,(uint32_t)strlen(s));
 if(width<0) width=0;
 return text((GAME_W-width)/2,top,px,color,s);
}
static void save_best(void) {
 if((uint32_t)game.score<=best) return;
 best=(uint32_t)game.score;
 int rc=aster_kv_set("best",4,&best,sizeof(best));
 if(rc<0) aster_errfmt("breakout save failed: %d",rc);
}
static void draw_ball_and_paddle(void) {
 const uint16_t mint=ASTER_RGB565(157,246,208),ink=ASTER_RGB565(239,245,234);
 aster_ge_rect_r(&ge,(int)game.paddle-PADDLE_W/2,PADDLE_Y,PADDLE_W,PADDLE_H,mint,unit(4),0);
 aster_ge_ellipse(&ge,(int)game.x,(int)game.y,BALL_R+unit(2),BALL_R+unit(2),ASTER_RGB565(37,73,81),0);
 aster_ge_ellipse(&ge,(int)game.x,(int)game.y,BALL_R,BALL_R,ink,0);
}
static void remember_frame(void) {
 painted=1;last_x=(int)game.x;last_y=(int)game.y;last_paddle=(int)game.paddle;
 last_mode=game.mode;last_score=game.score;last_lives=game.lives;force_full=0;
 memcpy(last_bricks,game.brick,sizeof(last_bricks));
}
static int render(void) {
 const uint16_t bg=ASTER_RGB565(12,20,34),panel=ASTER_RGB565(24,36,51);
 const uint16_t ink=ASTER_RGB565(239,245,234),muted=ASTER_RGB565(151,175,192);
 const uint16_t mint=ASTER_RGB565(157,246,208);
 char label[40];
 /* The adaptive canvas owns a persistent ordinary surface. Reconstruct only the
  * regions touched by moving objects; state/font changes repaint everything. */
 if(painted && !force_full && !panel_canvas && game.mode==PLAYING && last_mode==PLAYING &&
    game.score>=last_score && game.lives==last_lives) {
  const int pad=BALL_R+unit(4);
  aster_ge_rect(&ge,last_x-pad,last_y-pad,2*pad+1,2*pad+1,bg);
  aster_ge_rect(&ge,last_paddle-PADDLE_W/2-2,PADDLE_Y-2,PADDLE_W+4,PADDLE_H+4,bg);
  if(game.score!=last_score) {
   aster_ge_rect(&ge,sx(17),sy(58),sx(210),(uint32_t)aster_text_line_h(font_px(16)),bg);
   aster_snprintf(label,sizeof(label),"得分 %u",(unsigned)game.score);
   if(text(sx(17),sy(58),font_px(16),muted,label)<0) return ASTER_ENOSPC;
   for(int i=0;i<BRICKS;i++) if(last_bricks[i] && !game.brick[i]) {
    int x=BRICK_X+i%COLS*(BRICK_W+BRICK_GAP),y=BRICK_Y+i/COLS*BRICK_STEP_Y;
    aster_ge_rect(&ge,x-1,y-1,BRICK_W+2,BRICK_H+2,bg);
   }
  }
  for(int i=0;i<BRICKS;i++) if(game.brick[i]) {
   int x=BRICK_X+i%COLS*(BRICK_W+BRICK_GAP),y=BRICK_Y+i/COLS*BRICK_STEP_Y;
   if(x-1<=last_x+pad && x+BRICK_W+1>=last_x-pad &&
      y-1<=last_y+pad && y+BRICK_H+1>=last_y-pad) {
    aster_ge_rect(&ge,x-1,y-1,BRICK_W+2,BRICK_H+2,bg);
    aster_ge_rect_r(&ge,x,y,BRICK_W,BRICK_H,colors[i/COLS],unit(3),0);
   }
  }
  if(last_y-pad<=FIELD_T+1) {
   aster_ge_rect(&ge,FIELD_L-1,FIELD_T-1,FIELD_R-FIELD_L+3,3,bg);
   aster_ge_line_aa(&ge,FIELD_L,FIELD_T,FIELD_R,FIELD_T,ASTER_RGB565(43,62,77));
  }
  draw_ball_and_paddle();
  int rc=aster_ge_present(&ge);if(rc>=0) remember_frame();return rc;
 }
 aster_ge_clear(&ge,bg);
 if(text(sx(17),sy(12),font_px(20),ink,"打砖块")<0) return ASTER_ENOSPC;
 aster_ge_rect_r(&ge,sx(246),sy(12),sx(58),sy(48),panel,unit(10),0);
 if(game.mode==PAUSED) aster_ge_tri(&ge,sx(268),sy(24),sx(268),sy(45),sx(285),sy(35),mint);
 else {aster_ge_rect(&ge,sx(266),sy(24),unit(5),sy(22),mint);aster_ge_rect(&ge,sx(279),sy(24),unit(5),sy(22),mint);}
 aster_snprintf(label,sizeof(label),"得分 %u",(unsigned)game.score);
 if(text(sx(17),sy(58),font_px(16),muted,label)<0) return ASTER_ENOSPC;
 for(int i=0;i<3;i++) aster_ge_ellipse(&ge,sx(249+i*20),sy(74),unit(5),unit(5),
  i<game.lives?mint:panel,0);
 aster_ge_line_aa(&ge,FIELD_L,FIELD_T,FIELD_R,FIELD_T,ASTER_RGB565(43,62,77));
 for(int i=0;i<BRICKS;i++) if(game.brick[i]) {
  int x=BRICK_X+i%COLS*(BRICK_W+BRICK_GAP),y=BRICK_Y+i/COLS*BRICK_STEP_Y;
  aster_ge_rect_r(&ge,x,y,BRICK_W,BRICK_H,colors[i/COLS],unit(3),0);
 }
 draw_ball_and_paddle();
 if(game.mode==LOST || game.mode==WON || game.mode==PAUSED) {
  int overlay_h=aster_text_line_h(font_px(20))+aster_text_line_h(font_px(16))+unit(24);
  if(overlay_h<sy(90)) overlay_h=sy(90);
  int overlay_y=sy(321)-overlay_h/2;
  aster_ge_rect_r(&ge,sx(35),overlay_y,sx(250),overlay_h,panel,unit(12),0);
  const char *title=game.mode==WON?"全部击破！":game.mode==LOST?"再来一局":"已暂停";
  if(center(overlay_y+unit(10),font_px(20),ink,title)<0) return ASTER_ENOSPC;
  aster_snprintf(label,sizeof(label),"最高分 %u",best);
  if(center(overlay_y+font_px(16)+aster_text_line_h(font_px(20)),font_px(16),muted,label)<0) return ASTER_ENOSPC;
 }
 if(game.mode==PLAYING) {
  if(center(layout.action_y+(layout.action_h-aster_text_line_h(font_px(16)))/2,font_px(16),muted,"左右拖动挡板接球")<0) return ASTER_ENOSPC;
 } else {
  aster_ge_rect_r(&ge,sx(32),layout.action_y,sx(256),layout.action_h,mint,unit(10),0);
  const char *label=game.mode==READY?"点击发球":game.mode==PAUSED?"继续游戏":"重新开始";
  if(center(layout.action_y+(layout.action_h-aster_text_line_h(font_px(16)))/2,font_px(16),bg,label)<0) return ASTER_ENOSPC;
 }
 int rc=aster_ge_present(&ge);if(rc>=0) remember_frame();return rc;
}
static int create_canvas(void) {
 aster_display_info_t info={.size=sizeof(info),.version=1};
 int rc=aster_display_get_info(0,&info,sizeof(info));
 if(rc<0) return rc;
 if(layout_configure(info.width,info.height)<0) return ASTER_EINVAL;
 int button_min=aster_text_line_h(font_px(16))+unit(12);
 if(layout.action_h<button_min) layout.action_h=button_min;
 layout.action_y=GAME_H-unit(12)-layout.action_h;
 if(PADDLE_Y>layout.action_y-unit(25)) layout.paddle_y=layout.action_y-unit(25);
 int header_bottom=sy(58)+aster_text_line_h(font_px(16))+unit(6);
 if(FIELD_T<header_bottom) {layout.field_top=header_bottom;layout.brick_top=header_bottom+unit(12);}
 aster_printf("breakout display %ux%u, adaptive canvas %ux%u",info.width,info.height,GAME_W,GAME_H);
 aster_surface_desc_t desc={.size=sizeof(desc),.version=1,.width=GAME_W,.height=GAME_H,
  .format=ASTER_PIXEL_RGB565,
  .flags=(uint64_t)GAME_W*GAME_H>307200?ASTER_SURFACE_F_PANEL_CANVAS:ASTER_SURFACE_F_SCALE_FIT};
 rc=aster_surface_create(&desc,sizeof(desc),&surface);
 if(rc<0) return rc;
 aster_surface_buffer_t buffer={.size=sizeof(buffer),.version=1};
 rc=aster_surface_acquire(surface,&buffer,sizeof(buffer));
 if(rc<0) {cleanup();return rc;}
 panel_canvas=(buffer.flags&ASTER_SURFACE_F_PANEL_CANVAS)!=0;
 painted=0;force_full=1;
 aster_ge_init(&ge,surface,GAME_W,GAME_H,commands,160,arena,sizeof(arena));
 return ASTER_OK;
}
ASTER_APP_EXPORT int aster_app_init(void) {
 int rc=create_canvas();if(rc<0) return rc;
 /* Measure through the public font API during init to preload static labels. */
 const char *body="得分 0123456789最高分点击发球继续游戏重新开始左右拖动挡板接球";
 const char *titles="打砖块已暂停再来一局全部击破！";
 (void)aster_text_measure(font_px(16),body,(uint32_t)strlen(body));
 (void)aster_text_measure(font_px(20),titles,(uint32_t)strlen(titles));
 colors[0]=ASTER_RGB565(255,129,138);colors[1]=ASTER_RGB565(255,188,115);
 colors[2]=ASTER_RGB565(245,220,134);colors[3]=ASTER_RGB565(133,214,186);
 colors[4]=ASTER_RGB565(146,177,245);
 best=0;uint32_t saved=0;rc=aster_kv_get("best",4,&saved,sizeof(saved));
 if(rc==(int)sizeof(saved)) best=saved;
 game_new(&game);dirty=1;down_kind=0;saved_score=-1;last_ms=aster_time_now_ms();
 rc=aster_app_set_framerate(60);if(rc<0) {cleanup();return rc;}
 aster_printf("breakout ready bricks=%u",BRICKS);return ASTER_OK;
}
ASTER_APP_EXPORT void aster_app_event(const aster_event_t *ev) {
 if(!ev || !surface) return;
 if(ev->type==ASTER_EVENT_FRAME) {
  uint32_t now=aster_time_now_ms(),elapsed=now-last_ms;last_ms=now;
  int fonts=aster_gfx_fonts_pending();
  if(fonts) force_full=1;
  if(game.mode==PLAYING) {
   if(elapsed>64) elapsed=64;
   while(elapsed) {int step=elapsed>8?8:(int)elapsed;game_tick(&game,step);elapsed-=step;}
   dirty=1;
  }

  if((game.mode==LOST || game.mode==WON) && saved_score!=game.score) {
   saved_score=game.score;save_best();aster_printf("breakout round score=%u",(unsigned)game.score);
  }

  if(dirty || fonts) {
   int rc=render();if(rc<0) {aster_errfmt("breakout render failed: %d",rc);aster_app_exit(rc);return;}
   dirty=0;
  }
 } else if(ev->type==ASTER_EVENT_TOUCH && ev->u.touch.slot==0) {
  int x=(int)ev->u.touch.x,y=(int)ev->u.touch.y;
  if(ev->u.touch.action==ASTER_TOUCH_DOWN) {
   down_kind=x>=sx(246) && y<sy(66)?1:y>=layout.action_y?2:3;
   if(down_kind==3 && y>=sy(270) && (game.mode==PLAYING || game.mode==READY)) game_move(&game,(float)x);
  } else if(ev->u.touch.action==ASTER_TOUCH_MOVE) {
   if(down_kind==3 && y>=sy(270) && (game.mode==PLAYING || game.mode==READY)) game_move(&game,(float)x);
  } else if(ev->u.touch.action==ASTER_TOUCH_UP) {
   if(down_kind==1 && x>=sx(246) && y<sy(66)) {
    if(game.mode==PLAYING) game.mode=PAUSED;else if(game.mode==PAUSED) game.mode=PLAYING;
    aster_printf("breakout mode=%u",(unsigned)game.mode);
   } else if(down_kind==2 && y>=layout.action_y) {
    if(game.mode==LOST || game.mode==WON) {game_new(&game);saved_score=-1;}
    else if(game.mode==PAUSED) game.mode=PLAYING;
    else if(game.mode==READY) {game_launch(&game);aster_printf("breakout launch");}
   }
   if(down_kind==3) aster_printf("breakout paddle=%u",(unsigned)game.paddle);
   down_kind=0;
  }
  dirty=1;
 } else if(ev->type==ASTER_EVENT_KEY && ev->u.key.pressed && ev->u.key.code==1) {
  save_best();aster_app_exit(0);return;
 } else if(ev->type==ASTER_EVENT_SYS && ev->u.sys.code==ASTER_SYS_ORIENT) {
  int old_w=GAME_W,old_h=GAME_H;cleanup();int rc=create_canvas();
  if(rc<0) {aster_app_exit(rc);return;}
  game.x=game.x*GAME_W/old_w;game.y=game.y*GAME_H/old_h;
  game.vx=game.vx*GAME_W/old_w;game.vy=game.vy*GAME_H/old_h;
  game_move(&game,game.paddle*GAME_W/old_w);down_kind=0;dirty=1;
 }
}
ASTER_APP_EXPORT void aster_app_pause(void) {if(game.mode==PLAYING) game.mode=PAUSED;}
ASTER_APP_EXPORT void aster_app_resume(void) {down_kind=0;last_ms=aster_time_now_ms();dirty=1;}
ASTER_APP_EXPORT void aster_app_stop(void) {save_best();cleanup();}
ASTER_APP_EXPORT void aster_app_destroy(void) {cleanup();}
