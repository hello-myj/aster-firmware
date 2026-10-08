/* SPDX-License-Identifier: MIT
 * FLUX gesture sampling and time-based scrolling; no display dependency. */
#ifndef ASTER_FLUX_MOTION_H
#define ASTER_FLUX_MOTION_H
#include <stdint.h>
#include <stdbool.h>

#define FLUX_RIBBON_PITCH 106
#define FLUX_SNAP_MS 320

struct flux_velocity {
 int y, speed;
 uint64_t sampled, moved;
};
struct flux_scroll_plan {
 int target;
 unsigned duration;
 bool coast;
};
static inline int flux_limit(int v,int lo,int hi) {return v<lo?lo:v>hi?hi:v;}
static inline int flux_nearest(int v) {return (v+(v<0?-500:500))/1000;}
static inline void flux_velocity_reset(struct flux_velocity *v,int y,uint64_t now)
{
 *v=(struct flux_velocity){.y=y,.sampled=now,.moved=now};
}
static inline void flux_velocity_sample(struct flux_velocity *v,int y,uint64_t now)
{
 int dy=y-v->y;
 unsigned dt=(unsigned)(now-v->sampled);
 if(!dy) {if(now-v->moved>100) v->speed=0;return;}
 /* Accumulate very closely spaced events instead of dividing by 1 ms. */
 if(dt<16) return;
 int speed=flux_limit((int)((int64_t)dy*1000/dt),-1700,1700);
 if(!v->speed || (v->speed<0)!=(speed<0)) v->speed=speed;
 else v->speed=(v->speed+speed*2)/3;
 v->y=y;v->sampled=now;v->moved=now;
}
static inline int flux_velocity_release(const struct flux_velocity *v,uint64_t now)
{
 return now-v->moved>100?0:v->speed;
}
static inline struct flux_scroll_plan flux_scroll_plan(int phase,int pixels_per_second,int count)
{
 struct flux_scroll_plan p={.target=flux_nearest(phase)*1000,.duration=FLUX_SNAP_MS};
 int speed=flux_limit(-pixels_per_second*1000/FLUX_RIBBON_PITCH,-16000,16000);
 int magnitude=speed<0?-speed:speed;
 if(count<=1 || magnitude<1400) return p;
 p.duration=(unsigned)flux_limit(650+magnitude/12,650,1800);
 /* Cubic deceleration integrates to velocity * duration / 3. Keep the
  * endpoint unwrapped: shortest-path snapping reverses long circular flicks. */
 int travel=flux_limit(speed*(int)p.duration/3000,-8000,8000);
 p.target=flux_nearest(phase+travel)*1000;
 p.coast=(p.target-phase)*(int64_t)speed>0;
 return p;
}
static inline int flux_scroll_ease(uint64_t elapsed,unsigned duration,bool coast)
{
 int t=elapsed>=duration?1000:(int)(elapsed*1000/duration),q=1000-t;
 int64_t remaining=(int64_t)q*q*q/1000000;
 if(!coast) remaining=remaining*q/1000;
 return 1000-(int)remaining;
}
#endif
