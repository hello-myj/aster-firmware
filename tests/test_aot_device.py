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
static int aster_storage_app_code_dir(char *dir,size_t n,const char *id) {snprintf(dir,n,"/apps/%s",id);return 0;}
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
        code = source[source.index('static bool install_id_valid('):source.index('\t/* Signature gate BEFORE')]
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define LOG_ERR(...) ((void)0)
#define ASTER_ECORRUPT -14
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
 strcpy(m->id,"test.game");assert(img==image && n==8);*wasm=image;*wl=8;*aot=image;*al=aot_len;*at=target_id;return 0;
}
''' + code + r'''
 return ASTER_OK;
}
int main(void) {
 aot_len=0;target_id=1;assert(pkg_install_locked("test.ast")==ASTER_ENOTSUP && frees==1);
 aot_len=8;target_id=99;assert(pkg_install_locked("test.ast")==ASTER_EABI && frees==2);
 target_id=1;assert(pkg_install_locked("test.ast")==ASTER_OK && frees==2);
 return 0;
}
''')
