"""Exercise real device load/install gates without an interpreter fallback."""
import unittest
from test_waveshare import ROOT, run_c


class DeviceAOT(unittest.TestCase):
    def test_loader_rejects_missing_wrong_or_failed_aot_without_deleting_it(self):
        source = (ROOT / 'systems/zephyr/runtime/aster_runtime.c').read_text()
        code = source[source.index('static int ctx_load('):source.index('\tuint32_t stack_size = rec->mem_stack')]
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define IS_ENABLED(x) (x)
#define LOG_ERR(...) ((void)0)
static int CONFIG_ASTER_AOT=1;
enum {Wasm_Module_Bytecode,Wasm_Module_AoT};
typedef struct {const char *id;bool has_aot;} aster_app_rec_t;
typedef struct {uint8_t *img;size_t img_size;void *module;} aster_app_ctx_t;
static uint8_t image[8];
static int disk_error,module_error,loads,frees,kind=Wasm_Module_AoT,unlinks;
static void aster_storage_app_dir(char *dir,size_t n,const char *id) {snprintf(dir,n,"/apps/%s",id);}
static int load_image(const char *path,uint8_t **out,size_t *size) {
 assert(strstr(path,"/app.aot"));loads++;
 if(disk_error) return disk_error;
 *out=image;*size=sizeof(image);return 0;
}
static int get_package_type(const uint8_t *img,uint32_t n) {assert(img==image && n==8);return kind;}
static void *wasm_runtime_load(uint8_t *img,uint32_t n,char *error,size_t cap) {
 assert(img==image && n==8);snprintf(error,cap,"broken relocation");return module_error?NULL:(void *)image;
}
static void aster_psram_free(void *p) {assert(p==image);frees++;}
static int fs_unlink(const char *path) {(void)path;unlinks++;return 0;}
''' + code + r'''
 return 0;
}
int main(void) {
 aster_app_ctx_t ctx={0};aster_app_rec_t rec={"test.game",false};
 assert(ctx_load(&ctx,&rec)==-ENOTSUP && loads==0);
 rec.has_aot=true;CONFIG_ASTER_AOT=0;
 assert(ctx_load(&ctx,&rec)==-ENOTSUP && loads==0);CONFIG_ASTER_AOT=1;
 disk_error=-ENOENT;assert(ctx_load(&ctx,&rec)==-ENOENT && loads==1);disk_error=0;
 kind=Wasm_Module_Bytecode;assert(ctx_load(&ctx,&rec)==-EINVAL && frees==1 && !ctx.img);
 kind=Wasm_Module_AoT;module_error=1;
 assert(ctx_load(&ctx,&rec)==-EINVAL && frees==2 && !ctx.img);
 assert(loads==3 && unlinks==0); /* no WASM retry and installed image survives */
 module_error=0;assert(ctx_load(&ctx,&rec)==0 && ctx.module==image);
 assert(loads==4 && unlinks==0);
 return 0;
}
''')

    def test_install_rejects_missing_or_wrong_cpu_aot_before_writing(self):
        source = (ROOT / 'systems/zephyr/services/aster_pkg.c').read_text()
        code = source[source.index('int aster_pkg_install_file('):source.index('\t/* Signature gate BEFORE')]
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define LOG_ERR(...) ((void)0)
#define ASTER_OK 0
#define ASTER_EIO -7
#define ASTER_EABI -13
#define ASTER_ENOTSUP -3
#define CONFIG_ASTER_AOT_TARGET_ID 1
static uint8_t image[8];static size_t aot_len;static uint32_t target_id;static int frees;
typedef struct {char id[64];} aster_manifest_t;
static int load_file(const char *path,uint8_t **out,size_t *size) {(void)path;*out=image;*size=8;return 0;}
static void aster_psram_free(void *p) {assert(p==image);frees++;}
static int aster_pkg_parse(uint8_t *img,size_t n,aster_manifest_t *m,const uint8_t **wasm,size_t *wl,
                          const uint8_t **aot,size_t *al,uint32_t *at) {
 (void)m;assert(img==image && n==8);*wasm=image;*wl=8;*aot=image;*al=aot_len;*at=target_id;return 0;
}
''' + code + r'''
 return ASTER_OK;
}
int main(void) {
 aot_len=0;target_id=1;assert(aster_pkg_install_file("test.ast")==ASTER_ENOTSUP && frees==1);
 aot_len=8;target_id=99;assert(aster_pkg_install_file("test.ast")==ASTER_EABI && frees==2);
 target_id=1;assert(aster_pkg_install_file("test.ast")==ASTER_OK && frees==2);
 return 0;
}
''')

    def test_aot_extraction_open_and_short_writes_are_fatal(self):
        source = (ROOT / 'systems/zephyr/services/aster_pkg.c').read_text()
        start = source.index('\tif (aot_ok) {', source.index('int aster_pkg_install_file('))
        code = source[start:source.index('\t/* launcher tile:', start)]
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define LOG_ERR(...) ((void)0)
#define ASTER_EIO -7
struct fs_file_t {int unused;};
static int open_error,written=8,frees,closes;
static uint8_t image[8];
static void fs_file_t_init(struct fs_file_t *f) {(void)f;}
#define FS_O_WRITE 1
#define FS_O_CREATE 2
#define FS_O_TRUNC 4
static int fs_open(struct fs_file_t *f,const char *path,int flags) {(void)f;(void)path;assert(flags==7);return open_error;}
static int aster_fs_write_safe(struct fs_file_t *f,const void *p,size_t n) {(void)f;assert(p==image && n==8);return written;}
static void fs_close(struct fs_file_t *f) {(void)f;closes++;}
static void aster_psram_free(void *p) {assert(p==image);frees++;}
static int extract(void) {
 bool aot_ok=true;char dst[192];const char *appdir="/apps/test";
 struct fs_file_t f;int err;size_t alen=8;uint8_t *img=image;const uint8_t *aot=image;
''' + code + r'''
 return 0;
}
int main(void) {
 open_error=-1;assert(extract()==ASTER_EIO && frees==1 && closes==0);
 open_error=0;written=7;assert(extract()==ASTER_EIO && frees==2 && closes==1);
 written=-1;assert(extract()==ASTER_EIO && frees==3 && closes==2);
 written=8;assert(extract()==0 && frees==3 && closes==3);
 return 0;
}
''')
