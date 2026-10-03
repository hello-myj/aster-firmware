"""Exercise app-owned introductions, JSON persistence and old-app fallback."""
import unittest
from test_waveshare import ROOT, run_c


class Description(unittest.TestCase):
    def test_json_roundtrip_and_utf8_boundary(self):
        s = (ROOT / 'systems/zephyr/services/aster_pkg.c').read_text()
        parsing = s[s.index('static const char *find_key'):s.index('int aster_json_u32')]
        writing = s[s.index('static int json_string_field'):s.index('static int json_arr')]
        run_c(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ASTER_APP_DESCRIPTION_MAX 128
''' + parsing + writing + r'''
int main(void) {
 char out[128], json[1152]="{}", text[256];
 aster_pkg_read_description(json,out); assert(!out[0]);
 const char *expected="播放音乐：\"收藏\"，路径 C:\\Music";
 json[0]='{';
 int n=json_string_field(json+1,sizeof(json)-2,"description",expected);
 assert(n>0); /* remove leading comma for a one-key object */
 memmove(json+1,json+2,(size_t)n-1); json[n]='}'; json[n+1]=0;
 aster_pkg_read_description(json,out); assert(!strcmp(out,expected));
 assert(json_string_field(json,4,"description",expected)<0);
 memset(text,'a',126); memcpy(text+126,"乐",4);
 snprintf(json,sizeof(json),"{\"description\":\"%s\"}",text);
 aster_pkg_read_description(json,out); assert(strlen(out)==126);
 memset(text,'a',124); memcpy(text+124,"乐",4);
 snprintf(json,sizeof(json),"{\"description\":\"%s\"}",text);
 aster_pkg_read_description(json,out); assert(strlen(out)==127);
 return 0;
}
''')

    def test_launcher_uses_metadata_for_known_and_unknown_apps(self):
        s = (ROOT / 'systems/zephyr/ui/launcher/flux.c').read_text()
        types = s[s.index('typedef struct {'):s.index('static const char *const group_names')]
        read = s[s.index('static void read_entries'):s.index('static void filter_entries')]
        run_c(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#define ASTER_APP_DESCRIPTION_MAX 128
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
enum {FILES,MIXER,SYSTEM,CLOCK,ICON_BOX,ICON_FILES,ICON_SETTINGS,ICON_TERMINAL,ICON_CLOCK};
typedef struct {char id[64],name[40],category[16],description[128];} aster_app_rec_t;
static aster_app_rec_t recs[]={
 {.id="aster.player",.name="Player",.description="APP 自己的介绍"},
 {.id="org.example.dynamic",.name="动态应用",.category="tool",.description="独立安装的介绍"},
 {.id="org.example.old",.name="旧应用"}};
static int aster_app_mgr_count(void) {return 3;}
static const aster_app_rec_t *aster_app_mgr_get(int i) {return &recs[i];}
static const aster_app_rec_t *aster_app_mgr_find(const char *id) {
 for(int i=0;i<3;i++) if(!strcmp(recs[i].id,id)) return &recs[i];
 return NULL;
}
#define MAX_ENTRIES 20
''' + types + '\nstatic entry_t entries[MAX_ENTRIES];static int entry_n;\n' + read + r'''
int main(void) {
 read_entries(); assert(entry_n==7);
 assert(!strcmp(entries[0].description,"APP 自己的介绍"));
 assert(!strcmp(entries[1].description,"独立安装的介绍"));
 assert(!entries[2].description[0]);
 recs[1].description[0]=0; /* borrowed registry storage changed */
 assert(!strcmp(entries[1].description,"独立安装的介绍"));
 read_entries(); assert(!entries[1].description[0]);
 return 0;
}
''')
