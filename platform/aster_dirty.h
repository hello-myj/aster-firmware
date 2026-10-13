/* SPDX-License-Identifier: MIT
 * Bounded half-open dirty rectangles. No allocation and no lost updates.
 * Coordinates are clipped before insertion; all areas/costs use 64 bits.
 */
#ifndef ASTER_DIRTY_H
#define ASTER_DIRTY_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#ifndef CONFIG_ASTER_GFX_DIRTY_REGIONS
#define CONFIG_ASTER_GFX_DIRTY_REGIONS 8
#endif
#define ASTER_DIRTY_CAP CONFIG_ASTER_GFX_DIRTY_REGIONS
typedef struct { uint32_t x, y, w, h; } aster_dirty_rect_t;
typedef struct { uint32_t count; aster_dirty_rect_t rect[ASTER_DIRTY_CAP]; } aster_dirty_set_t;
static inline uint64_t aster_dirty_area(aster_dirty_rect_t r) { return (uint64_t)r.w*r.h; }
static inline aster_dirty_rect_t aster_dirty_union(aster_dirty_rect_t a,aster_dirty_rect_t b) {
 uint32_t x=a.x<b.x?a.x:b.x, y=a.y<b.y?a.y:b.y;
 uint32_t x1=a.x+a.w>b.x+b.w?a.x+a.w:b.x+b.w;
 uint32_t y1=a.y+a.h>b.y+b.h?a.y+a.h:b.y+b.h;
 return (aster_dirty_rect_t){x,y,x1-x,y1-y};
}
/* One extra window is traded for at most this many additional pixels.
 * 32 pixels = 64 bytes, deliberately conservative until board measurements.
 */
#define ASTER_DIRTY_WINDOW_PIXELS 32u
static inline uint64_t aster_dirty_cost(aster_dirty_rect_t a,aster_dirty_rect_t b) {
 uint64_t combined=aster_dirty_area(aster_dirty_union(a,b));
 uint64_t separate=aster_dirty_area(a)+aster_dirty_area(b);
 return combined>separate?combined-separate:0;
}
static inline void aster_dirty_add(aster_dirty_set_t *s,uint32_t width,uint32_t height,
 uint32_t x,uint32_t y,uint32_t w,uint32_t h) {
 if(!w||!h||x>=width||y>=height) return;
 if(w>width-x) w=width-x;
 if(h>height-y) h=height-y;
 aster_dirty_rect_t r={x,y,w,h};
 /* Merge only when the resulting bounding box is cheap, including overlap.
  * Thin crossing rectangles may overlap yet have an expensive union. */
 for(uint32_t i=0;i<s->count;) {
  if(aster_dirty_cost(r,s->rect[i])<=ASTER_DIRTY_WINDOW_PIXELS) {
   r=aster_dirty_union(r,s->rect[i]);s->rect[i]=s->rect[--s->count];i=0;
  } else i++;
 }
 if(s->count<ASTER_DIRTY_CAP) {s->rect[s->count++]=r;return;}
 /* Capacity: merge the cheapest pair among existing rectangles and the new
  * one. A bounding box always covers both, so exhaustion cannot drop pixels. */
 uint64_t best=UINT64_MAX;uint32_t ai=0,bi=0;
 for(uint32_t i=0;i<s->count;i++) {
  uint64_t cost=aster_dirty_cost(s->rect[i],r);
  if(cost<best) {best=cost;ai=i;bi=s->count;}
  for(uint32_t j=i+1;j<s->count;j++) {
   cost=aster_dirty_cost(s->rect[i],s->rect[j]);
   if(cost<best) {best=cost;ai=i;bi=j;}
  }
 }
 if(bi==s->count) s->rect[ai]=aster_dirty_union(s->rect[ai],r);
 else {s->rect[ai]=aster_dirty_union(s->rect[ai],s->rect[bi]);s->rect[bi]=r;}
}
static inline bool aster_dirty_is_full(const aster_dirty_set_t *s,uint32_t w,uint32_t h) {
 return s->count==1 && s->rect[0].x==0 && s->rect[0].y==0 && s->rect[0].w==w && s->rect[0].h==h;
}
static inline void aster_dirty_append(aster_dirty_set_t *s,const aster_dirty_set_t *batch,
 uint32_t w,uint32_t h) {
 for(uint32_t i=0;i<batch->count;i++) {aster_dirty_rect_t r=batch->rect[i];aster_dirty_add(s,w,h,r.x,r.y,r.w,r.h);}
}
#endif
