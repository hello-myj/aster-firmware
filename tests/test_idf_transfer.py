"""Exercise actual ASTERFS receiver with fragmented binary IO and real files."""
import unittest
from test_waveshare import ROOT, run_c


class Transfer(unittest.TestCase):
    def test_binary_crc_resume_and_publish(self):
        source = ROOT / 'systems/espidf/components/aster_transfer/transfer.c'
        run_c('#define _POSIX_C_SOURCE 200809L\n#include "' + str(source) + '"\n' + r'''
#include <assert.h>
#include <stdlib.h>
typedef struct {const uint8_t *data;size_t size,position;char last[64];unsigned acks;} channel;
static int receive(void *p,uint8_t *out,size_t size) {
 channel *c=p;if(c->position==c->size)return -ETIMEDOUT;
 if(size>17)size=17;
 if(size>c->size-c->position)size=c->size-c->position;
 memcpy(out,c->data+c->position,size);c->position+=size;return (int)size;
}
static void reply(void *p,const char *line) {
 channel *c=p;snprintf(c->last,sizeof(c->last),"%s",line);
 if(!strncmp(line,"ACK ",4))c->acks++;
}
static void expect_file(const char *path,const uint8_t *data,size_t size) {
 FILE *f=fopen(path,"rb");assert(f);uint8_t actual[4096];
 assert(fread(actual,1,sizeof(actual),f)==size);assert(!memcmp(actual,data,size));fclose(f);
}
int main(void) {
 char mapped[192];
 assert(!aster_transfer_package_path("/SD:/test.ast",mapped,sizeof(mapped)) && !strcmp(mapped,"/SD/test.ast"));
 assert(!aster_transfer_package_path("/SD:/aster/install/test.ast",mapped,sizeof(mapped)) && !strcmp(mapped,"/SD/aster/install/test.ast"));
 assert(aster_transfer_package_path("/SD:/aster/apps/app.wasm",mapped,sizeof(mapped)));
 assert(aster_transfer_package_path("/SD:/../test.ast",mapped,sizeof(mapped)));
 assert(aster_transfer_package_path("/SD:/test\\bad.ast",mapped,sizeof(mapped)));
 assert(aster_transfer_package_path("/SD:/test.ast",mapped,3));
 assert(aster_transfer_crc32(0,(const uint8_t *)"123456789",9)==0xcbf43926);
 uint32_t c=aster_transfer_crc32(0,(const uint8_t *)"1234",4);
 assert(aster_transfer_crc32(c,(const uint8_t *)"56789",5)==0xcbf43926);
 char dir[]="/tmp/aster-transfer-XXXXXX";assert(mkdtemp(dir));
 char file[192],part[208];snprintf(file,sizeof(file),"%s/app.ast",dir);snprintf(part,sizeof(part),"%s.part",file);
 uint8_t data[4096];for(unsigned i=0;i<sizeof(data);i++)data[i]=(uint8_t)(i*37);
 uint32_t crc=aster_transfer_crc32(0,data,sizeof(data));
 channel ch={.data=data,.size=sizeof(data)};
 aster_transfer_io_t io={receive,reply,&ch};
 assert(!aster_transfer_receive(file,sizeof(data),crc,0,&io));
 assert(!strcmp(ch.last,"OK") && ch.acks==8);expect_file(file,data,sizeof(data));
 /* Interrupted transfer cannot replace the old verified file. */
 data[0]^=0x7f;crc=aster_transfer_crc32(0,data,sizeof(data));
 ch=(channel){.data=data,.size=700};
 assert(aster_transfer_receive(file,sizeof(data),crc,0,&io));
 assert(!strcmp(ch.last,"RESUME 512"));
 data[0]^=0x7f;expect_file(file,data,sizeof(data));data[0]^=0x7f;
 /* Overclaimed resume is clamped to the actual .part size. */
 ch=(channel){.data=data+512,.size=sizeof(data)-512};
 assert(!aster_transfer_receive(file,sizeof(data),crc,2048,&io));
 expect_file(file,data,sizeof(data));
 ch=(channel){.data=data,.size=sizeof(data)};
 assert(aster_transfer_receive(file,sizeof(data),crc^1,0,&io));
 assert(!strcmp(ch.last,"ERR retry") && access(part,F_OK));expect_file(file,data,sizeof(data));
 ch=(channel){.data=data};assert(aster_transfer_receive(file,0,crc,0,&io));
 assert(!strcmp(ch.last,"ERR -22"));
 /* Entire valid .part is published without asking the host for a body. */
 FILE *f=fopen(part,"wb");assert(f && fwrite(data,1,sizeof(data),f)==sizeof(data));fclose(f);
 ch=(channel){0};assert(!aster_transfer_receive(file,sizeof(data),crc,sizeof(data),&io));
 assert(!strcmp(ch.last,"OK") && !ch.acks);expect_file(file,data,sizeof(data));
 unlink(file);rmdir(dir);return 0;
}
''')
