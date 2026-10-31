/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
static uint32_t le32(const uint8_t *p)
{return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
/* A compatibility/profile check, not verification of native machine code.
 * AOT input must come from a trusted compiler/package source. */
bool aster_idf_aot_interruptible(const uint8_t *bytes,size_t size)
{
    if(size<8 || memcmp(bytes,"\0aot",4) || le32(bytes+4)!=6)return false;
    size_t offset=8;bool threaded=false;
    while(offset<=size && size-offset>=8) {
        uint32_t kind=le32(bytes+offset),length=le32(bytes+offset+4);offset+=8;
        if(length>size-offset)return false;
        const uint8_t *body=bytes+offset;
        if(kind==0){if(length<24)return false;threaded=(le32(body+16)&4)!=0;}
        if(kind==1) {
            if(length<24 || !threaded || le32(body)!=0 || le32(body+4)!=1 || le32(body+8)!=3)return false;
            /* WAMR folds fixed memories into one enlarged internal page when
             * memory.grow is absent; this unit is not always 64 KiB in AOT. */
            uint32_t unit=le32(body+12),initial=le32(body+16),maximum=le32(body+20);
            return initial<=maximum && unit>0 && maximum>0 && (uint64_t)unit*maximum<=16*1024*1024;
        }
        offset+=length;
        if(offset>SIZE_MAX-3)return false;
        offset=(offset+3)&~(size_t)3;
    }
    return false;
}
