"""Run the real installer and registry reader over fault-injected host files.

This validates error propagation/reboot visibility, not SD/FAT power-loss semantics.
"""
import unittest
from test_waveshare import ROOT, run_c


class InstallIO(unittest.TestCase):
    def program(self):
        pkg = (ROOT / 'systems/zephyr/services/aster_pkg.c').read_text()
        header = (ROOT / 'systems/zephyr/services/aster_pkg.h').read_text()
        manager = (ROOT / 'systems/zephyr/services/aster_app_mgr.c').read_text()
        manifest = header[header.index('typedef struct {'):header.index('} aster_manifest_t;') + len('} aster_manifest_t;')]
        bounds = pkg[pkg.index('static bool in_bounds('):pkg.index('int aster_pkg_parse(')]
        installer = pkg[pkg.index('static int json_string_field('):]
        reader = manager[manager.index('static int read_manifest_at('):manager.index('/* Record fields')]
        version_header = (ROOT / 'systems/zephyr/services/aster_versions.h').read_text()
        version_header = '\n'.join(line for line in version_header.splitlines() if not line.startswith('#include'))
        versions = (ROOT / 'systems/zephyr/services/aster_versions.c').read_text()
        versions = versions[versions.index('static uint32_t version_crc_update'):]
        storage = (ROOT / 'systems/zephyr/services/aster_storage.c').read_text()
        code_dir = storage[storage.index('int aster_storage_app_code_dir('):storage.index('int aster_storage_sandbox(')]
        sandbox = storage[storage.index('int aster_storage_sandbox('):]
        music = storage[storage.index('bool aster_storage_is_music_path('):storage.index('int aster_storage_init(')]
        return (r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#define ASTER_OK 0
#define ASTER_EIO -7
#define ASTER_EBUSY -5
#define ASTER_ENOMEM -2
#define ASTER_ECORRUPT -14
#define ASTER_EABI -13
#define ASTER_ENOTSUP -3
#define ASTER_MANIFEST_MAX 1152
#define ASTER_APP_DESCRIPTION_MAX 128
#define ASTER_INSTALL_MARKER ".installing"
#define ASTER_ORIENT_ANY -1
#define CONFIG_ASTER_AOT_TARGET_ID 1
#define FS_O_WRITE 1
#define FS_O_CREATE 2
#define FS_O_TRUNC 4
#define FS_O_READ 8
#define FS_DIR_ENTRY_FILE 1
#define FS_DIR_ENTRY_DIR 2
static char root[64],last_log[1024],error_log[1024];
static const char *fail_path,*fail_op;
static unsigned operation_count,fail_at,crash_at,revision=1;
static const char *package_id="test.app";
static unsigned lock_depth;static bool busy,manifest_oom;static int pinned_slot=-2;
static bool aster_app_mgr_code_slot(const char *id,int *slot) {(void)id;if(pinned_slot < -1)return false;*slot=pinned_slot;return true;}
static void aster_app_mgr_files_lock(void) {lock_depth++;}
static void aster_app_mgr_files_unlock(void) {assert(lock_depth);lock_depth--;}
static bool aster_app_mgr_is_live(const char *id) {(void)id;assert(lock_depth);return busy;}
static unsigned opens,closes,allocated,freed,installed;
static void log_message(const char *fmt,...) {
 va_list ap;va_start(ap,fmt);vsnprintf(last_log,sizeof(last_log),fmt,ap);va_end(ap);
 if(!strncmp(fmt,"installed ",10)) installed++;
}
static void log_error(const char *fmt,...) {
 va_list ap;va_start(ap,fmt);vsnprintf(error_log,sizeof(error_log),fmt,ap);va_end(ap);
}
#define LOG_ERR(...) log_error(__VA_ARGS__)
#define LOG_WRN(...) log_message(__VA_ARGS__)
#define LOG_INF(...) log_message(__VA_ARGS__)
#define ASTER_APPS_DIR root
#define ASTER_MOUNT_POINT root
#define ASTER_MUSIC_DIR "/music"
#define ASTER_PERM_SD (1u << 3)
struct fs_file_t {FILE *stream;char path[512];};
struct fs_dir_t {DIR *dir;};
struct fs_dirent {char name[256];unsigned type;size_t size;};
static bool fault(const char *op,const char *path) {
 operation_count++;
 return (fail_at && operation_count==fail_at) ||
        (fail_op && !strcmp(op,fail_op) && strstr(path,fail_path));
}
static int result(int value) {
 if(crash_at && operation_count==crash_at) _exit(77);
 return value;
}
static void fs_file_t_init(struct fs_file_t *f) {memset(f,0,sizeof(*f));}
static int fs_open(struct fs_file_t *f,const char *path,int flags) {
 if(fault("open",path)) return result(-ENOSPC);
 snprintf(f->path,sizeof(f->path),"%s",path);
 f->stream=fopen(path,flags==FS_O_READ?"rb":"wb");
 if(!f->stream) return result(-errno);
 opens++;return result(0);
}
static int aster_fs_write_safe(struct fs_file_t *f,const void *data,size_t len) {
 if(fault("write",f->path)) {
  if(len) fwrite(data,1,len-1,f->stream);
  return result(len?(int)len-1:-ENOSPC);
 }
 size_t written=fwrite(data,1,len,f->stream);
 if(fail_op && !strcmp(fail_op,"corrupt") && strstr(f->path,fail_path) && written) {
  assert(!fseek(f->stream,-1,SEEK_CUR));fputc(((const uint8_t *)data)[len-1]^0x80,f->stream);
 }
 return result((int)written);
}
static int fs_sync(struct fs_file_t *f) {
 if(fault("sync",f->path)) return result(-EIO);
 return result(fflush(f->stream)?-errno:0);
}
static int fs_close(struct fs_file_t *f) {
 bool bad=fault("close",f->path);closes++;int err=fclose(f->stream);
 return result(bad?-EIO:err?-errno:0);
}
static int fs_read(struct fs_file_t *f,void *buf,size_t len) {
 return result((int)fread(buf,1,fault("read",f->path)&&len?len-1:len,f->stream));
}
#define aster_fs_read_safe fs_read
static int fs_stat(const char *path,struct fs_dirent *entry) {
 if(fault("stat",path)) return result(-EIO);
 struct stat st;if(stat(path,&st)) return result(-errno);
 entry->type=S_ISDIR(st.st_mode)?FS_DIR_ENTRY_DIR:FS_DIR_ENTRY_FILE;
 entry->size=(size_t)st.st_size;return result(0);
}
static void fs_dir_t_init(struct fs_dir_t *d) {d->dir=NULL;}
static int fs_opendir(struct fs_dir_t *d,const char *path) {
 if(fault("opendir",path)) return result(-EIO);
 d->dir=opendir(path);return result(d->dir?0:-errno);
}
static int fs_readdir(struct fs_dir_t *d,struct fs_dirent *entry) {
 if(fault("readdir","directory")) return result(-EIO);
 struct dirent *de;
 while((de=readdir(d->dir))) {
  if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
  snprintf(entry->name,sizeof(entry->name),"%s",de->d_name);
  struct stat st;assert(!fstatat(dirfd(d->dir),de->d_name,&st,0));
  entry->type=S_ISDIR(st.st_mode)?FS_DIR_ENTRY_DIR:FS_DIR_ENTRY_FILE;
  return result(0);
 }
 entry->name[0]=0;return result(0);
}
static int fs_closedir(struct fs_dir_t *d) {
 bool bad=fault("closedir","directory");int err=closedir(d->dir);
 return result(bad?-EIO:err?-errno:0);
}
static int fs_unlink(const char *path) {
 if(fault("unlink",path)) return result(-EACCES);
 struct stat st;if(stat(path,&st)) return result(-errno);
 return result((S_ISDIR(st.st_mode)?rmdir(path):unlink(path))?-errno:0);
}
static int aster_storage_mkdir_p(const char *path) {
 if(fault("mkdir",path)) return result(-ENOSPC);
 char buffer[512];snprintf(buffer,sizeof(buffer),"%s",path);
 for(char *p=buffer+1;*p;p++) if(*p=='/') {
  *p=0;int err=mkdir(buffer,0700);*p='/';if(err&&errno!=EEXIST) return result(-errno);
 }
 return result(!mkdir(buffer,0700)||errno==EEXIST?0:-errno);
}
static void aster_storage_app_dir(char *out,size_t cap,const char *id) {snprintf(out,cap,"%s/%s",root,id);}
static void *aster_psram_alloc(size_t size) {if(manifest_oom)return NULL;void *p=malloc(size);if(p)allocated++;return p;}
static void aster_psram_free(void *p) {freed++;free(p);}
static uint16_t aster_le16(const void *p) {const uint8_t *b=p;return b[0]|(uint16_t)b[1]<<8;}
static uint32_t aster_le32(const void *p) {const uint8_t *b=p;return b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;}
static int load_file(const char *path,uint8_t **out,size_t *len) {
 (void)path;*out=calloc(1,256);assert(*out);*len=256;allocated++;
 memcpy(*out,"\0asm",4);memcpy(*out+8,"\0aot",4);(*out)[4]=(uint8_t)revision;(*out)[12]=(uint8_t)revision;(*out)[16]=(uint8_t)revision;
 uint8_t *a=*out+64;memcpy(a,"ASTR",4);a[4]=1;a[6]=1;
 a[16]=28;a[20]=40;a[24]=8;memcpy(a+28,"test.bin",9);a[40]=(uint8_t)revision;return 0;
}
static int aster_sig_check(const uint8_t *img,size_t n) {(void)img;(void)n;return 0;}
static int aster_icon_parse(const void *img,size_t n,void *out) {(void)img;(void)n;(void)out;return 0;}
static const char *aster_pkg_orientation_name(int8_t o) {(void)o;return NULL;}
''' + manifest + r'''
static int aster_pkg_parse(const uint8_t *img,size_t n,aster_manifest_t *m,
 const uint8_t **wasm,size_t *wlen,const uint8_t **aot,size_t *alen,uint32_t *target) {
 assert(n==256);memset(m,0,sizeof(*m));snprintf(m->id,sizeof(m->id),"%s",package_id);strcpy(m->name,"Test");snprintf(m->version,sizeof(m->version),"%u.0",revision);
 strcpy(m->abi_min,"3.7");strcpy(m->abi_target,"3.7.0");m->orientation=ASTER_ORIENT_ANY;
 m->icon_off=16;m->icon_len=8;m->hdr_size=0;m->assets_off=64;m->assets_len=64;
 *wasm=img;*wlen=8;*aot=img+8;*alen=8;*target=1;return 0;
}
''' + bounds + version_header + '\n' + versions + code_dir + music + sandbox + installer + reader + r'''
static void fresh_root(void) {
 strcpy(root,"/tmp/aster-install-io-XXXXXX");assert(mkdtemp(root));
 opens=closes=allocated=freed=installed=0;busy=manifest_oom=false;assert(!lock_depth);error_log[0]=0;fail_op=fail_path=NULL;operation_count=fail_at=crash_at=0;revision=1;pinned_slot=-2;
}

static void user_file(const char *sub,const char *value) {
 char path[160],base[128];aster_storage_app_dir(base,sizeof(base),"test.app");
 snprintf(path,sizeof(path),"%s/%s",base,sub);
 char parent[160];snprintf(parent,sizeof(parent),"%s",path);*strrchr(parent,'/')=0;
 assert(!aster_storage_mkdir_p(parent));FILE *f=fopen(path,"wb");assert(f);
 assert(fwrite(value,1,strlen(value),f)==strlen(value));assert(!fclose(f));
}
static void expect_file(const char *path,const void *bytes,size_t size) {
 uint8_t out[256];FILE *f=fopen(path,"rb");assert(f && size<=sizeof(out));
 assert(fread(out,1,sizeof(out),f)==size && !memcmp(out,bytes,size));assert(!fclose(f));
}
static void check_user_data(void) {
 char path[160];snprintf(path,sizeof(path),"%s/test.app/data/save.bin",root);expect_file(path,"save-42",7);
 snprintf(path,sizeof(path),"%s/test.app/cache/persist.bin",root);expect_file(path,"cache-42",8);
}
static void prepare_upgrade(void) {
 fresh_root();assert(!aster_pkg_install_file("v1.ast"));
 revision=2;assert(!aster_pkg_install_file("v2.ast"));
 user_file("data/save.bin","save-42");user_file("cache/persist.bin","cache-42");
 revision=3;operation_count=0;
}
static unsigned selected_revision(void) {
 char manifest_buf[1152],code[128],path[160];uint8_t bytes[8];
 assert(!read_manifest_for("test.app",manifest_buf,sizeof(manifest_buf)));
 assert(!aster_storage_app_code_dir(code,sizeof(code),"test.app"));
 unsigned selected=strstr(manifest_buf,"\"version\":\"3.0\"")?3:
                   strstr(manifest_buf,"\"version\":\"2.0\"")?2:1;
 snprintf(path,sizeof(path),"%s/app.aot",code);FILE *f=fopen(path,"rb");assert(f);
 assert(fread(bytes,1,8,f)==8 && !memcmp(bytes,"\0aot",4) && bytes[4]==selected);assert(!fclose(f));
 snprintf(path,sizeof(path),"%s/app.wasm",code);f=fopen(path,"rb");assert(f);
 assert(fread(bytes,1,8,f)==8 && !memcmp(bytes,"\0asm",4) && bytes[4]==selected);assert(!fclose(f));
 memset(bytes,0,sizeof(bytes));bytes[0]=(uint8_t)selected;
 snprintf(path,sizeof(path),"%s/icon.bin",code);expect_file(path,bytes,sizeof(bytes));
 snprintf(path,sizeof(path),"%s/assets/test.bin",code);expect_file(path,bytes,sizeof(bytes));
 check_user_data();return selected;
}
''')

    def test_each_payload_open_write_sync_close_and_commit_failure_is_visible(self):
        run_c(self.program() + r'''
int main(void) {
 const char *paths[]={".installing","package.ast","app.wasm","app.aot","icon.bin","test.bin","manifest.json"};
 const char *ops[]={"open","write","sync","close"};char manifest_buf[1152];
 for(unsigned p=0;p<sizeof(paths)/sizeof(paths[0]);p++) {
  for(unsigned op=0;op<sizeof(ops)/sizeof(ops[0]);op++) {
   fresh_root();fail_path=paths[p];fail_op=ops[op];
   assert(aster_pkg_install_file("input.ast")==ASTER_EIO);
   assert(installed==0 && allocated==freed && opens==closes && !lock_depth);
   assert(strstr(error_log,"stage=")&&strstr(error_log,"operation="));
   fail_op=NULL;
   assert(read_manifest_for("test.app",manifest_buf,sizeof(manifest_buf))!=0);
   /* A reboot simply reconstructs the registry from these same files. */
   assert(aster_pkg_install_file("retry.ast")==0 && installed==1);
   assert(read_manifest_for("test.app",manifest_buf,sizeof(manifest_buf))==0);
   assert(strstr(manifest_buf,"\"installed\":1")&&strstr(manifest_buf,"\"aot\":1"));
   assert(opens==closes && allocated==freed && !lock_depth);assert(!rmrf(root));
  }
 }
 for(unsigned i=0;i<2;i++) {
  fresh_root();fail_path=i?".installing":"/data";fail_op=i?"unlink":"mkdir";
  assert(aster_pkg_install_file("input.ast")==ASTER_EIO && installed==0);
  fail_op=NULL;assert(read_manifest_for("test.app",manifest_buf,sizeof(manifest_buf))!=0);
  assert(!rmrf(root));
 }
 fresh_root();assert(!aster_pkg_install_file("input.ast"));
 fail_op="read";fail_path="manifest.json";assert(read_manifest_for("test.app",manifest_buf,sizeof(manifest_buf))!=0);
 fail_op="close";assert(read_manifest_for("test.app",manifest_buf,sizeof(manifest_buf))!=0);
 fail_op="stat";fail_path=".installing";assert(read_manifest_for("test.app",manifest_buf,sizeof(manifest_buf))!=0);
 fail_op=NULL;assert(!rmrf(root));
 return 0;
}
''')

    def test_upgrade_each_io_error_and_process_crash_keeps_a_complete_version_and_data(self):
        run_c(self.program() + r"""
int main(void) {
 prepare_upgrade();assert(!aster_pkg_install_file("v3.ast"));unsigned steps=operation_count;
 assert(selected_revision()==3);assert(!rmrf(root));
 for(unsigned step=1;step<=steps;step++) {
  prepare_upgrade();fail_at=step;
  int err=aster_pkg_install_file("v3.ast");fail_at=0;
  unsigned selected=selected_revision();assert(selected==2||selected==3);
  if(!err) assert(selected==3);
  assert(!lock_depth && opens==closes && allocated==freed);
  assert(!aster_pkg_install_file("retry-v3.ast"));assert(selected_revision()==3);assert(!rmrf(root));
 }
 for(unsigned step=1;step<=steps;step++) {
  prepare_upgrade();pid_t child=fork();assert(child>=0);
  if(!child) {crash_at=step;int err=aster_pkg_install_file("v3.ast");_exit(err?1:0);}
  int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status));
  assert(WEXITSTATUS(status)==77||WEXITSTATUS(status)==0);
  unsigned selected=selected_revision();assert(selected==2||selected==3);
  assert(!aster_pkg_install_file("retry-v3.ast"));assert(selected_revision()==3);assert(!rmrf(root));
 }
 printf("Upgrade fault matrix: %u I/O failure points and %u abrupt process exits; user data preserved.\n",steps,steps);
 return 0;
}
""", timeout=30)

    def test_corrupt_payload_is_rejected_and_torn_record_recovers_previous_version(self):
        run_c(self.program() + r"""
int main(void) {
 prepare_upgrade();fail_path="app.aot";fail_op="corrupt";
 assert(aster_pkg_install_file("v3.ast")==ASTER_ECORRUPT);fail_op=NULL;
 assert(selected_revision()==2);assert(!aster_pkg_install_file("v3.ast"));assert(selected_revision()==3);
 char path[160];snprintf(path,sizeof(path),"%s/test.app/.active0",root);
 /* A partial metadata overwrite must not select new/incomplete content. */
 FILE *f=fopen(path,"wb");assert(f);fwrite("ASTV",1,4,f);fclose(f);
 assert(selected_revision()==2);
 assert(!aster_pkg_install_file("retry.ast"));assert(selected_revision()==3);
 /* Flip a byte while retaining the record length: CRC must reject it. */
 f=fopen(path,"r+b");assert(f);assert(!fseek(f,8,SEEK_SET));int b=fgetc(f);assert(b>=0);
 assert(!fseek(f,8,SEEK_SET));fputc(b^0x80,f);fclose(f);assert(selected_revision()==2);
 assert(!rmrf(root));return 0;
}
""")

    def test_legacy_upgrade_preserves_data_and_live_apps_prevent_mutation(self):
        run_c(self.program() + r"""
int main(void) {
 prepare_upgrade();assert(selected_revision()==2);busy=true;
 assert(aster_pkg_install_file("v3.ast")==ASTER_EBUSY);busy=false;assert(selected_revision()==2);
 assert(!lock_depth);assert(!rmrf(root));
 /* Migrate a flat-layout installation without moving or copying its data. */
 fresh_root();assert(!aster_pkg_install_file("v1.ast"));
 char slot[160],base[128],from[192],to[192];aster_storage_app_dir(base,sizeof(base),"test.app");
 snprintf(slot,sizeof(slot),"%s/.v0",base);
 const char *names[]={"package.ast","app.aot","app.wasm","icon.bin","manifest.json","assets"};
 for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
  snprintf(from,sizeof(from),"%s/%s",slot,names[i]);snprintf(to,sizeof(to),"%s/%s",base,names[i]);assert(!rename(from,to));
 }
 snprintf(to,sizeof(to),"%s/.active0",base);assert(!unlink(to));assert(!rmdir(slot));
 user_file("data/save.bin","save-42");user_file("cache/persist.bin","cache-42");
 assert(selected_revision()==1);revision=2;
 fail_path="app.aot";fail_op="write";assert(aster_pkg_install_file("v2.ast")==ASTER_EIO);fail_op=NULL;
 assert(selected_revision()==1);assert(!aster_pkg_install_file("v2.ast"));assert(selected_revision()==2);
 /* Delayed/torn new metadata can fall back to the retained legacy files. */
 snprintf(to,sizeof(to),"%s/.active0",base);assert(!unlink(to));assert(selected_revision()==1);
 assert(!rmrf(root));return 0;
}
""")

    def test_code_assets_and_data_paths_stay_coherent_and_sequence_wrap_recovers(self):
        run_c(self.program() + r"""
int main(void) {
 prepare_upgrade();assert(!aster_pkg_install_file("v3.ast"));
 char path[160],expected[160],base[128],slot[128];
 assert(!aster_storage_sandbox(path,sizeof(path),"test.app","/data/save.bin",0));
 snprintf(expected,sizeof(expected),"%s/test.app/data/save.bin",root);assert(!strcmp(path,expected));
 assert(!aster_storage_sandbox(path,sizeof(path),"test.app","/cache/persist.bin",0));
 snprintf(expected,sizeof(expected),"%s/test.app/cache/persist.bin",root);assert(!strcmp(path,expected));
 assert(!aster_storage_sandbox(path,sizeof(path),"test.app","/assets/test.bin",0));
 snprintf(expected,sizeof(expected),"%s/test.app/.v0/assets/test.bin",root);assert(!strcmp(path,expected));
 /* Cached slot and metadata remain coherent even if a pointer read fails. */
 pinned_slot=1;fail_op="stat";fail_path=".active1";assert(selected_revision()==2);
 fail_op=NULL;pinned_slot=-2;assert(selected_revision()==3);
 aster_storage_app_dir(base,sizeof(base),"test.app");
 uint8_t record[ASTER_VERSION_RECORD_SIZE];uint32_t crc;
 assert(!aster_versions_dir(slot,sizeof(slot),base,0));assert(!aster_versions_manifest_crc(slot,&crc));
 aster_versions_record(record,0,UINT32_MAX,crc);snprintf(path,sizeof(path),"%s/.active0",base);
 assert(!install_write(path,record,sizeof(record),"test"));
 assert(!aster_versions_dir(slot,sizeof(slot),base,1));assert(!aster_versions_manifest_crc(slot,&crc));
 aster_versions_record(record,1,UINT32_MAX-1,crc);snprintf(path,sizeof(path),"%s/.active1",base);
 assert(!install_write(path,record,sizeof(record),"test"));
 assert(selected_revision()==3);revision=4;assert(!aster_pkg_install_file("v4.ast"));
 struct aster_version selected;assert(!aster_versions_find(base,&selected));
 assert(selected.slot==1 && selected.sequence==0);check_user_data();
 assert(!rmrf(root));return 0;
}
""")

    def test_registry_refresh_remains_locked_after_success_or_uncertain_commit(self):
        source = (ROOT / 'systems/zephyr/services/aster_app_mgr.c').read_text()
        code = source[source.index('int aster_app_mgr_install('):source.index('static int rmrf(')]
        run_c(r"""
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define ASTER_EIO -7
#define LOG_ERR(...) snprintf(logbuf,sizeof(logbuf),__VA_ARGS__)
static char logbuf[128];
static int depth,result,scanned,scan_error;static bool live;
static void aster_app_mgr_files_lock(void) {depth++;}
static void aster_app_mgr_files_unlock(void) {assert(depth);depth--;}
static bool aster_app_mgr_is_live(const char *id) {(void)id;assert(depth);return live;}
static int aster_pkg_install_file(const char *path) {(void)path;assert(depth);return result;}
static int aster_app_mgr_scan(void) {assert(depth);scanned++;return scan_error;}
""" + code + r"""
int main(void) {
 assert(!aster_app_mgr_install("test.ast") && scanned==1 && !depth);
 result=-7;assert(aster_app_mgr_install("test.ast")==-7 && scanned==2 && !depth);
 live=true;result=-5;assert(aster_app_mgr_install("test.ast")==-5 && scanned==2 && !depth);
 live=false;result=0;scan_error=-28;assert(aster_app_mgr_install("test.ast")==-7 && scanned==3 && !depth);
 return 0;
}
""")

    def test_invalid_package_id_cannot_modify_any_version_or_user_directory(self):
        run_c(self.program() + r"""
int main(void) {
 const char *bad[]={"../test.app",".v0","test/app","test\\app","test..app","../data"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
  prepare_upgrade();package_id=bad[i];
  assert(aster_pkg_install_file("evil.ast")==ASTER_ECORRUPT);
  package_id="test.app";assert(selected_revision()==2);
  assert(!lock_depth && allocated==freed && opens==closes);assert(!rmrf(root));
 }
 return 0;
}
""")

    def test_manifest_buffer_allocation_failure_preserves_previous_version(self):
        run_c(self.program() + r"""
int main(void) {
 prepare_upgrade();manifest_oom=true;
 assert(aster_pkg_install_file("v3.ast")==ASTER_ENOMEM);
 manifest_oom=false;assert(selected_revision()==2);
 assert(!lock_depth && allocated==freed && opens==closes);
 assert(!aster_pkg_install_file("retry.ast"));assert(selected_revision()==3);
 assert(!rmrf(root));return 0;
}
""")
