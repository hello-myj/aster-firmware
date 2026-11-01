/* SPDX-License-Identifier: MIT */
/* ASTERFS 3 stop-and-wait receiver, independent of the UART/console API. */
#include "transfer.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
int aster_transfer_package_path(const char *remote,char *out,size_t capacity)
{
    static const char root[]="/SD:/",install[]="/SD:/aster/install/";
    const char *name=NULL;
    if(!strncmp(remote,install,sizeof(install)-1))name=remote+sizeof(install)-1;
    else if(!strncmp(remote,root,sizeof(root)-1))name=remote+sizeof(root)-1;
    size_t n=name?strlen(name):0;
    if(!name || n<5 || n>80 || strstr(name,"..") || strpbrk(name,"/\\: \t\r\n") || strcmp(name+n-4,".ast"))return -EPERM;
    int length=snprintf(out,capacity,"/SD%s",remote+4);
    return length<0 || (size_t)length>=capacity?-ENAMETOOLONG:0;
}
uint32_t aster_transfer_crc32(uint32_t crc,const uint8_t *data,size_t size)
{
    crc=~crc;
    for(size_t i=0;i<size;i++) {
        crc^=data[i];
        for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^((0u-(crc&1))&0xedb88320u);
    }
    return ~crc;
}
static uint32_t part_size(const char *path)
{
    struct stat st;return stat(path,&st)==0 && st.st_size>=0 && st.st_size<=UINT32_MAX?(uint32_t)st.st_size:0;
}
static int token(const aster_transfer_io_t *io,const char *kind,uint32_t value)
{
    char line[64];snprintf(line,sizeof(line),"%s %lu",kind,(unsigned long)value);io->reply(io->context,line);return 1;
}
static int error(const aster_transfer_io_t *io,int code)
{char line[64];snprintf(line,sizeof(line),"ERR %d",code);io->reply(io->context,line);return 1;}
static int finish(FILE *file)
{
    int result=fflush(file);
    if(!result)result=fsync(fileno(file));
    if(fclose(file))result=-1;
    return result;
}
int aster_transfer_receive(const char *path,uint32_t length,uint32_t crc,uint32_t offset,const aster_transfer_io_t *io)
{
    if(!length || length>32u*1024u*1024u || offset>length)return error(io,-EINVAL);
    char temporary[208];
    if(snprintf(temporary,sizeof(temporary),"%s.part",path)>=(int)sizeof(temporary))return error(io,-ENAMETOOLONG);
    uint32_t have=offset?part_size(temporary):0;
    if(offset>have)offset=have;
    FILE *file=fopen(temporary,offset?"r+b":"wb");
    if(!file)return error(io,-errno);
    if(ftruncate(fileno(file),offset) || fseek(file,offset,SEEK_SET)) {
        fclose(file);return error(io,-EIO);
    }
    uint8_t buffer[512];uint32_t position=offset;
    if(position<length)token(io,"ACK",position);
    while(position<length) {
        size_t chunk=length-position;if(chunk>sizeof(buffer))chunk=sizeof(buffer);
        size_t got=0;
        while(got<chunk) {
            int read=io->read(io->context,buffer+got,chunk-got);
            if(read<=0 || (size_t)read>chunk-got) {
                (void)finish(file);return token(io,"RESUME",part_size(temporary));
            }
            got+=(size_t)read;
        }
        if(fwrite(buffer,1,chunk,file)!=chunk || fflush(file)) {
            (void)finish(file);return token(io,"RESUME",part_size(temporary));
        }
        position+=(uint32_t)chunk;
        if(position<length)token(io,"ACK",position);
    }
    if(finish(file))return token(io,"RESUME",part_size(temporary));
    file=fopen(temporary,"rb");
    if(!file)return token(io,"RESUME",position);
    uint32_t actual=0,total=0;size_t count;
    while((count=fread(buffer,1,sizeof(buffer),file))!=0) {
        actual=aster_transfer_crc32(actual,buffer,count);total+=(uint32_t)count;
    }
    int bad_read=ferror(file);fclose(file);
    if(bad_read)return token(io,"RESUME",position);
    if(total!=length || actual!=crc) {
        unlink(temporary);io->reply(io->context,"ERR retry");return 1;
    }
    /* FAT does not replace existing names on rename. Keep the verified
     * partial for retry if publication fails; installed APP slots are never
     * upload destinations. Existing packages survive CRC/receive failures. */
    struct stat st;
    if(stat(path,&st)==0 && unlink(path))return error(io,-errno);
    if(rename(temporary,path))return error(io,-errno);
    io->reply(io->context,"OK");return 0;
}
