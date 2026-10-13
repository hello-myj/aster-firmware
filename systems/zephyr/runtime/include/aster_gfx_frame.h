/* SPDX-License-Identifier: MIT */
#ifndef ASTER_GFX_FRAME_STATE_H
#define ASTER_GFX_FRAME_STATE_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <aster/gfx.h>
#include <aster/error.h>
enum { ASTER_FRAME_FREE, ASTER_FRAME_DRAWING, ASTER_FRAME_QUEUED, ASTER_FRAME_IN_FLIGHT };
typedef struct {
 uint8_t *pixels;
 uint32_t capacity, cache_surface, surface, id, sequence, w, h, batches, commands, arena;
 uint64_t deadline;
 int32_t error;
 uint8_t state;
 bool canvas, initialized;
} aster_frame_state_t;
static inline int32_t aster_frame_fail(aster_frame_state_t *f,int32_t error) {
 if(f->state && !f->error) f->error=error;
 return f->state ? f->error : error;
}
static inline int32_t aster_frame_check(aster_frame_state_t *f,uint64_t now) {
 if(f->state!=ASTER_FRAME_DRAWING) return ASTER_ESTATE;
 if(now>=f->deadline) aster_frame_fail(f,ASTER_ETIMEDOUT);
 return f->error;
}
static inline void aster_frame_abort(aster_frame_state_t *f) {
 f->state=ASTER_FRAME_FREE;f->error=0;f->surface=0;f->initialized=false;
}
static inline void aster_frame_release_cache(aster_frame_state_t *f,void (*release)(void *)) {
 aster_frame_abort(f);
 if(f->pixels) release(f->pixels);
 f->pixels=NULL;f->capacity=0;f->cache_surface=0;
}
static inline int32_t aster_frame_start(aster_frame_state_t *f,uint32_t surface,
 uint32_t w,uint32_t h,bool canvas,const uint8_t *source,uint32_t pitch,uint64_t now,
 void *(*allocate)(size_t),void (*release)(void *)) {
 if(f->state) return ASTER_EBUSY;
 if(!w||!h||(uint64_t)w*h>ASTER_GFX_FRAME_MAX_PIXELS||(!canvas&&(!source||pitch<w))) return ASTER_EINVAL;
 if(f->sequence==UINT32_MAX) return ASTER_ENOSPC;
 uint32_t bytes=w*h*2u;
 if(f->capacity<bytes) {
  uint8_t *next=allocate(bytes);if(!next) return ASTER_ENOMEM;
  if(f->pixels) release(f->pixels);
  f->pixels=next;f->capacity=bytes;
 }
 if(canvas) memset(f->pixels,0,bytes);
 else for(uint32_t y=0;y<h;y++) memcpy(f->pixels+y*w*2u,source+y*pitch*2u,w*2u);
 f->cache_surface=surface;f->surface=surface;f->w=w;f->h=h;f->canvas=canvas;f->initialized=!canvas;
 f->id=++f->sequence;f->deadline=now+ASTER_GFX_FRAME_TIMEOUT_MS;
 f->batches=f->commands=f->arena=0;f->error=0;f->state=ASTER_FRAME_DRAWING;
 return 0;
}
static inline int32_t aster_frame_batch(aster_frame_state_t *f,uint32_t surface,
 const aster_gfx_cmd_t *cmds,uint32_t n,uint32_t arena_len,uint64_t now) {
 int32_t e=aster_frame_check(f,now);if(e) return e;
 if(surface!=f->surface) return aster_frame_fail(f,ASTER_EBUSY);
 if(f->batches>=ASTER_GFX_FRAME_MAX_BATCHES||n>ASTER_GFX_FRAME_MAX_COMMANDS-f->commands||arena_len>ASTER_GFX_FRAME_MAX_ARENA-f->arena)
  return aster_frame_fail(f,ASTER_ENOSPC);
 if(!f->initialized && (!n||ASTER_GFX_OP(&cmds[0])!=ASTER_GFX_CLEAR)) return aster_frame_fail(f,ASTER_ESTATE);
 for(uint32_t i=0;i<n;i++) {
  uint32_t op=ASTER_GFX_OP(&cmds[i]);
  if(op>ASTER_GFX_BMP_SA||op==8u) return aster_frame_fail(f,ASTER_ENOTSUP);
  if(op==ASTER_GFX_TEXT && (cmds[i].w[3]>arena_len||ASTER_GFX_U16(cmds[i].w[4])>arena_len-cmds[i].w[3])) return aster_frame_fail(f,ASTER_EINVAL);
 }
 f->initialized=true;f->batches++;f->commands+=n;f->arena+=arena_len;return 0;
}
#endif
