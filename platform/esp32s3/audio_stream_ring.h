/* SPDX-License-Identifier: MIT. Caller supplies storage and synchronization. */
#ifndef ASTER_AUDIO_STREAM_RING_H
#define ASTER_AUDIO_STREAM_RING_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
struct audio_stream_ring { uint8_t *data; size_t cap, head, used; };
static size_t audio_ring_write(struct audio_stream_ring *r,const void *src,size_t n)
{
    if(n>r->cap-r->used)n=r->cap-r->used;
    if(!n)return 0;
    size_t pos=(r->head+r->used)%r->cap,first=r->cap-pos;
    if(first>n)first=n;
    memcpy(r->data+pos,src,first);memcpy(r->data,(const uint8_t *)src+first,n-first);
    r->used+=n;return n;
}
static size_t audio_ring_read(struct audio_stream_ring *r,void *dst,size_t n)
{
    if(n>r->used)n=r->used;
    if(!n)return 0;
    size_t first=r->cap-r->head;if(first>n)first=n;
    memcpy(dst,r->data+r->head,first);memcpy((uint8_t *)dst+first,r->data,n-first);
    r->head=(r->head+n)%r->cap;r->used-=n;return n;
}
#endif
