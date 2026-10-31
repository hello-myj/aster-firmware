/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdio.h>
#include <errno.h>
#include <dirent.h>
#include <stdint.h>
#include <sys/types.h>
#define FS_O_READ 1
#define FS_O_WRITE 2
#define FS_O_CREATE 4
#define FS_O_TRUNC 8
#define FS_O_APPEND 16
#define FS_SEEK_SET SEEK_SET
#define FS_SEEK_CUR SEEK_CUR
#define FS_DIR_ENTRY_FILE 1
#define FS_DIR_ENTRY_DIR 2
struct fs_file_t { FILE *file; };
struct fs_dirent { int type; size_t size; char name[256]; };
static inline void fs_file_t_init(struct fs_file_t *f) { f->file=NULL; }
int fs_open(struct fs_file_t *f,const char *path,int flags);
int fs_close(struct fs_file_t *f);
int fs_seek(struct fs_file_t *f,off_t offset,int whence);
int fs_stat(const char *path,struct fs_dirent *entry);

struct fs_dir_t { DIR *dir; char path[512]; };
static inline void fs_dir_t_init(struct fs_dir_t *d) { d->dir=NULL; }
int fs_opendir(struct fs_dir_t *d,const char *path);
int fs_readdir(struct fs_dir_t *d,struct fs_dirent *entry);
int fs_closedir(struct fs_dir_t *d);
int fs_mkdir(const char *path);
int fs_unlink(const char *path);
int fs_rename(const char *from,const char *to);
int fs_read(struct fs_file_t *f,void *p,size_t n);
int fs_write(struct fs_file_t *f,const void *p,size_t n);
int fs_sync(struct fs_file_t *f);
