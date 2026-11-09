/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "aster_mem.h"
#include "aster_storage.h"
#ifndef ASTER_IDF_HOST_TEST
#include "esp_vfs_fat.h"
int fs_statvfs(const char *path,struct fs_statvfs *stat)
{
    if(strcmp(path,ASTER_MOUNT_POINT))return -EINVAL;
    uint64_t total,free_bytes;
    if(esp_vfs_fat_info("/SD",&total,&free_bytes)!=ESP_OK)return -EIO;
    *stat=(struct fs_statvfs){.f_bsize=512,.f_frsize=512,.f_blocks=total/512,.f_bfree=free_bytes/512};return 0;
}
#endif

static const char *real_path(const char *path,char *out,size_t size)
{
    if (strncmp(path,"/SD:",4)==0) {
        if(path[4] && path[4]!='/') {errno=EINVAL;return NULL;}
        const char *root="/SD";
#ifdef ASTER_IDF_HOST_TEST
        /* Host regression tests use a private temporary volume. */
        root=getenv("ASTER_TEST_SD_ROOT");
        if(!root || !*root) {errno=EINVAL;return NULL;}
#endif
        int n=snprintf(out,size,"%s%s",root,path+4);
        if (n<0 || n>=(int)size) {errno=ENAMETOOLONG;return NULL;}
        return out;
    }
    return path;
}
int fs_open(struct fs_file_t *f,const char *path,int flags)
{
    if (flags & ~(FS_O_READ|FS_O_WRITE|FS_O_CREATE|FS_O_TRUNC|FS_O_APPEND)) return -ENOTSUP;
    char mapped[512]; path=real_path(path,mapped,sizeof(mapped));
    if (!path) return -errno;
    const char *mode=(flags&FS_O_WRITE) ? ((flags&FS_O_APPEND)?"a+b":(flags&FS_O_TRUNC)?"w+b":"r+b") : "rb";
    f->file=fopen(path,mode);
    if (!f->file && errno==ENOENT && (flags&FS_O_CREATE)) f->file=fopen(path,"w+b");
    return f->file ? 0 : -errno;
}
int fs_close(struct fs_file_t *f)
{
    if (!f->file) return -EBADF;
    int r=fclose(f->file);f->file=NULL;return r ? -errno : 0;
}
int fs_seek(struct fs_file_t *f,off_t off,int whence)
{ if (!f->file) return -EBADF;return fseeko(f->file,off,whence)==0 ? 0 : -errno; }
int fs_stat(const char *path,struct fs_dirent *entry)
{
    char mapped[512];path=real_path(path,mapped,sizeof(mapped));
    if (!path) return -errno;
    struct stat st;
    if (stat(path,&st)) return -errno;
    memset(entry,0,sizeof(*entry));entry->size=st.st_size;
    entry->type=S_ISDIR(st.st_mode)?FS_DIR_ENTRY_DIR:FS_DIR_ENTRY_FILE;
    return 0;
}
int aster_fs_read_safe(struct fs_file_t *f,void *dst,size_t size)
{
    if (!f->file) return -EBADF;
    /* FAT/SD DMA never sees a PSRAM destination. */
    uint8_t *bounce=heap_caps_malloc(4096,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA);
    if (!bounce) return -ENOMEM;
    size_t done=0;
    while (done<size) {
        size_t chunk=size-done;if(chunk>4096)chunk=4096;
        size_t n=fread(bounce,1,chunk,f->file);
        memcpy((uint8_t *)dst+done,bounce,n);done+=n;
        if(n<chunk)break;
    }
    free(bounce);
    return ferror(f->file) ? -EIO : (int)done;
}

void *aster_psram_alloc(size_t n) { return heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); }
void aster_psram_free(void *p) { free(p); }
void *aster_dram_alloc(size_t n) { return heap_caps_malloc(n,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT); }
void aster_dram_free(void *p) { free(p); }

#include <lvgl.h>
#ifndef ASTER_IDF_HOST_TEST
#include "render_workers.h"
#endif
void aster_idf_render_init(void)
{
    static bool ready;
    if (!ready) { lv_init();ready=true;
#ifndef ASTER_IDF_HOST_TEST
        aster_idf_render_workers_init();
#endif
    }
}

void aster_ui_lvgl_assert(void) { fputs("Aster LVGL assertion failed\n",stderr);abort(); }

int fs_read(struct fs_file_t *f,void *p,size_t n)
{ if(!f->file)return -EBADF;size_t got=fread(p,1,n,f->file);return got?(int)got:ferror(f->file)?-EIO:0; }
int fs_write(struct fs_file_t *f,const void *p,size_t n)
{ if(!f->file)return -EBADF;size_t got=fwrite(p,1,n,f->file);return got?(int)got:ferror(f->file)?-EIO:0; }
int fs_sync(struct fs_file_t *f)
{ if(!f->file)return -EBADF;return fflush(f->file)||fsync(fileno(f->file)) ? -errno : 0; }
int aster_fs_write_safe(struct fs_file_t *f,const void *data,size_t size)
{
    uint8_t *bounce=heap_caps_malloc(4096,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA);
    if(!bounce)return -ENOMEM;
    size_t done=0;int error=0;
    while(done<size) {
        size_t n=size-done;if(n>4096)n=4096;
        memcpy(bounce,(const uint8_t *)data+done,n);
        int wrote=fs_write(f,bounce,n);
        if(wrote<0){error=wrote;break;}done+=wrote;if((size_t)wrote<n)break;
    }
    free(bounce);return done?(int)done:error;
}
int fs_opendir(struct fs_dir_t *d,const char *path)
{
    char mapped[512];path=real_path(path,mapped,sizeof(mapped));if(!path)return -errno;
    if(strlen(path)>=sizeof(d->path))return -ENAMETOOLONG;
    strcpy(d->path,path);d->dir=opendir(path);return d->dir?0:-errno;
}
int fs_readdir(struct fs_dir_t *d,struct fs_dirent *entry)
{
    if(!d->dir)return -EBADF;
    errno=0;struct dirent *e;
    do {e=readdir(d->dir);} while(e && (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")));
    memset(entry,0,sizeof(*entry));
    if(!e)return errno?-errno:0;
    if(strlen(e->d_name)>=sizeof(entry->name))return -ENAMETOOLONG;
    strcpy(entry->name,e->d_name);char path[768];
    if(snprintf(path,sizeof(path),"%s/%s",d->path,e->d_name)>=(int)sizeof(path))return -ENAMETOOLONG;
    struct stat st;if(stat(path,&st))return -errno;
    entry->size=st.st_size;entry->type=S_ISDIR(st.st_mode)?FS_DIR_ENTRY_DIR:FS_DIR_ENTRY_FILE;return 0;
}
int fs_closedir(struct fs_dir_t *d)
{ if(!d->dir)return -EBADF;int r=closedir(d->dir);d->dir=NULL;return r?-errno:0; }
int fs_mkdir(const char *path)
{ char out[512];path=real_path(path,out,sizeof(out));return !path?-errno:mkdir(path,0755)?-errno:0; }
int fs_unlink(const char *path)
{
    char out[512];path=real_path(path,out,sizeof(out));if(!path)return -errno;
    struct stat st;if(stat(path,&st))return -errno;
    return (S_ISDIR(st.st_mode)?rmdir(path):unlink(path))?-errno:0;
}
int fs_rename(const char *from,const char *to)
{
    char a[512],b[512];from=real_path(from,a,sizeof(a));to=real_path(to,b,sizeof(b));
    return !from||!to?-errno:rename(from,to)?-errno:0;
}
