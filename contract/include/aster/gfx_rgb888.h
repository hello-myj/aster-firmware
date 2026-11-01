/* SPDX-License-Identifier: MIT */
#ifndef ASTER_GFX_RGB888_ABI_H
#define ASTER_GFX_RGB888_ABI_H
#include "gfx.h"
/* Optional protocol, negotiated using the existing FRAME_QUERY envelope.
 * RGB888_SUPPORTED does NOT imply transaction/frame support. Old hosts reject
 * the query/batch; clients must query before using this namespace. No imports
 * or legacy RGB565 opcodes change. Colors are full 0xRRGGBB, opacity 0..255.
 * BATCH: gfx_exec(surface,(aster_gfx_cmd_t*)commands,BATCH,arena,arena_len,
 *                 command_count,1,0,0). Commands are 32 bytes, max 256.
 * RGB888 surfaces use R,G,B bytes; BGR888 surfaces use B,G,R bytes.
 * Atomic validation precedes drawing. Successful batches present the surface;
 * this is immediate-mode drawing, with no transaction/rollback guarantee.
 */
#define ASTER_GFX_RGB888_SUPPORTED 8u
#define ASTER_GFX_RGB888_BATCH 0xfffffffeu
typedef struct {uint32_t w[8];} aster_gfx_rgb888_cmd_t;
enum {
 ASTER_RGB888_CLEAR=0, /* w3=color; w7=opacity */
 ASTER_RGB888_RECT=1,  /* w1=xy,w2=wh,w3=color,w4=radius,w6=border width(0 fill) */
 ASTER_RGB888_GRAD=2,  /* w1=xy,w2=wh,w3=c0,w4=c1,w6=horizontal(0/1) */
 ASTER_RGB888_TEXT=3,  /* w1=x/baseline,w2=px,w3=fg,w4=bg|0x1000000 (optional),w5=arena offset,w6=len */
 ASTER_RGB888_LINE=4,  /* w1=p0,w2=p1,w3=color,w4=width */
 ASTER_RGB888_CIRCLE=5,/* w1=center,w2=radius,w3=color,w4=border width(0 fill) */
 ASTER_RGB888_TRI=6,   /* w1=p0,w2=p1,w3=color,w4=p2 */
};
#endif
