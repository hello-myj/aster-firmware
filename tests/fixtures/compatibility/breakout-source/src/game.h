#ifndef BREAKOUT_GAME_H
#define BREAKOUT_GAME_H
#include <stdint.h>
#define COLS 6
#define ROWS 5
#define BRICKS (COLS * ROWS)
typedef struct {int width,height,field_top,brick_top,paddle_y,action_y,action_h;} layout_t;
static layout_t layout;
static int sx(int x) {return (x*layout.width+160)/320;}
static int sy(int y) {return (y*layout.height+240)/480;}
static int unit(int n) {
 int a=sx(n),b=sy(n),v=a<b?a:b;return v>0?v:1;
}
/* Native geometry; large surfaces request the firmware panel canvas. */
static int layout_configure(int width,int height) {
 if(width<120 || height<160 || width>4096 || height>4096) return -1;
 layout.width=width;layout.height=height;
 layout.field_top=sy(96);layout.brick_top=sy(112);
 layout.paddle_y=sy(395);layout.action_y=sy(420);layout.action_h=sy(48);return 0;
}
#define GAME_W (layout.width)
#define GAME_H (layout.height)
#define FIELD_L sx(16)
#define FIELD_R sx(304)
#define FIELD_T (layout.field_top)
#define BRICK_X sx(19)
#define BRICK_Y (layout.brick_top)
#define BRICK_W sx(42)
#define BRICK_H sy(18)
#define BRICK_GAP sx(6)
#define BRICK_STEP_Y sy(25)
#define PADDLE_Y (layout.paddle_y)
#define PADDLE_W sx(72)
#define PADDLE_H sy(12)
#define BALL_R unit(5)

enum { READY, PLAYING, PAUSED, LOST, WON };
typedef struct {
 float x,y,vx,vy,paddle;
 uint8_t brick[BRICKS];
 int mode,score,lives,left;
} game_t;
static float bounded(float v,float lo,float hi) {return v<lo?lo:v>hi?hi:v;}
static void game_dock(game_t *g) {g->x=g->paddle;g->y=PADDLE_Y-BALL_R-unit(3);g->vx=sx(90);g->vy=-sy(198);}
static void game_new(game_t *g) {
 g->paddle=GAME_W/2;g->score=0;g->lives=3;g->left=BRICKS;g->mode=READY;
 for(int i=0;i<BRICKS;i++) g->brick[i]=1;
 game_dock(g);
}
static void game_move(game_t *g,float x) {
 g->paddle=bounded(x,FIELD_L+PADDLE_W/2,FIELD_R-PADDLE_W/2);
 if(g->mode==READY) game_dock(g);
}
static void game_launch(game_t *g) {if(g->mode==READY) g->mode=PLAYING;}
static void game_tick(game_t *g,int ms) {
 if(g->mode!=PLAYING) return;
 float dt=ms/1000.0f,oldx=g->x,oldy=g->y;
 g->x+=g->vx*dt;g->y+=g->vy*dt;
 if(g->x<FIELD_L+BALL_R) {g->x=FIELD_L+BALL_R;g->vx=-g->vx;}
 if(g->x>FIELD_R-BALL_R) {g->x=FIELD_R-BALL_R;g->vx=-g->vx;}
 if(g->y<FIELD_T+BALL_R) {g->y=FIELD_T+BALL_R;g->vy=-g->vy;}
 if(g->vy>0 && oldy+BALL_R<=PADDLE_Y && g->y+BALL_R>=PADDLE_Y &&
    g->x+BALL_R>=g->paddle-PADDLE_W/2 && g->x-BALL_R<=g->paddle+PADDLE_W/2) {
  float offset=bounded((g->x-g->paddle)/(PADDLE_W/2),-1,1);
  g->y=PADDLE_Y-BALL_R;g->vx=offset*sx(170);
  if(g->vx>-sx(24) && g->vx<sx(24)) g->vx=offset<0?-sx(24):sx(24);
  g->vy=-sy(218);
 }
 for(int i=0;i<BRICKS;i++) {
  if(!g->brick[i]) continue;
  int bx=BRICK_X+(i%COLS)*(BRICK_W+BRICK_GAP),by=BRICK_Y+(i/COLS)*BRICK_STEP_Y;
  if(g->x+BALL_R<bx || g->x-BALL_R>bx+BRICK_W ||
     g->y+BALL_R<by || g->y-BALL_R>by+BRICK_H) continue;
  g->brick[i]=0;g->left--;g->score+=10+(ROWS-i/COLS)*5;
  if(oldy+BALL_R<=by) {g->y=by-BALL_R;g->vy=-g->vy;}
  else if(oldy-BALL_R>=by+BRICK_H) {g->y=by+BRICK_H+BALL_R;g->vy=-g->vy;}
  else if(oldx<bx) {g->x=bx-BALL_R;g->vx=-g->vx;}
  else {g->x=bx+BRICK_W+BALL_R;g->vx=-g->vx;}
  if(!g->left) g->mode=WON;
  break;
 }
 if(g->y-BALL_R>layout.action_y) {
  g->lives--;
  if(!g->lives) g->mode=LOST;
  else {g->mode=READY;game_dock(g);}
 }
}
#endif
