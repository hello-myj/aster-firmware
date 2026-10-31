/* SPDX-License-Identifier: MIT */
#include <aster/aster.h>
#include <stddef.h>
void *memset(void *p,int v,size_t n) {unsigned char *d=p;while(n--)*d++=v;return p;}
void *memcpy(void *p,const void *q,size_t n) {unsigned char *d=p;const unsigned char *s=q;while(n--)*d++=*s++;return p;}
#define CHECK(c,n) do {if(!(c)){aster_app_exit(n);return n;}}while(0)
static aster_handle_t once,repeat,leaked;
static unsigned repeated,ones,frames;
static int test_files(void)
{
    aster_handle_t f,d;char bytes[32]={0};uint32_t directory;
    CHECK(aster_fs_open("/data/value",11,ASTER_FS_O_RDWR|ASTER_FS_O_CREAT|ASTER_FS_O_TRUNC,&f)==0,10);
    CHECK(aster_fs_write(f,"abcdef",6)==6,11);
    CHECK(aster_fs_seek(f,-3,ASTER_FS_SEEK_END)==0,12);
    CHECK(aster_fs_read(f,bytes,3)==3 && bytes[0]=='d' && bytes[2]=='f',13);
    CHECK(aster_fs_read(f,(void *)0xfffffff0u,32)==ASTER_EINVAL,14);
    CHECK(aster_fs_close(f)==0 && aster_fs_close(f)==ASTER_EBADF,15);
    CHECK(aster_fs_open("/data/value",11,ASTER_FS_O_RDONLY,&f)==0,16);
    CHECK(aster_fs_write(f,"x",1)==ASTER_EPERM && aster_fs_close(f)==0,17);
    aster_stat_t st;
    CHECK(aster_fs_stat("/data/value",11,&st,sizeof(st))==0 && st.filesize==6,18);
    CHECK(aster_fs_open("/assets/bad",11,ASTER_FS_O_WRONLY|ASTER_FS_O_CREAT,&f)==ASTER_EPERM,19);
    CHECK(aster_fs_open("/music/bad",10,ASTER_FS_O_WRONLY|ASTER_FS_O_CREAT,&f)==ASTER_EPERM,20);
    CHECK(aster_fs_open("/sd/private",11,ASTER_FS_O_RDONLY,&f)==ASTER_EPERM,21);
    CHECK(aster_fs_open("/data/../../escape",18,ASTER_FS_O_RDONLY,&f)==ASTER_EINVAL,22);
    CHECK(aster_fs_opendir("/data",5,&d)==0,23);
    CHECK(aster_fs_dir_next(d,bytes,1,&directory)==ASTER_EMSGSIZE,24);
    CHECK(aster_fs_dir_next(d,bytes,sizeof(bytes),&directory)==5 && bytes[0]=='v' && !directory,25);
    CHECK(aster_fs_closedir(d)==0 && aster_fs_closedir(d)==ASTER_EBADF,26);
    CHECK(aster_kv_set("setting",7,"abcdef",6)==0,27);
    CHECK(aster_kv_get("setting",7,bytes,2)==6 && bytes[0]=='a' && bytes[1]=='b',28);
    CHECK(aster_kv_get("setting",7,0,0)==6,29);
    CHECK(aster_kv_set("../bad",6,"x",1)==ASTER_EINVAL,30);
    CHECK(aster_kv_delete("setting",7)==0 && aster_kv_get("setting",7,bytes,2)==ASTER_ENOENT,31);
    /* Leave handles open deliberately: the host repeats this VM 12 times. */
    CHECK(aster_fs_open("/data/value",11,ASTER_FS_O_RDONLY,&leaked)==0,32);
    CHECK(aster_fs_opendir("/data",5,&d)==0,33);
    return 0;
}
int32_t aster_app_init(void)
{
    aster_handle_t temp;
    aster_time_info_t clock;
    CHECK(aster_time_get(&clock,sizeof(clock))==0 && clock.version==1 && clock.unix_ms>1704067200000ull,43);
    CHECK(aster_time_get((void *)0xfffffff0u,sizeof(clock))==ASTER_EINVAL,44);
    CHECK(aster_time_get(&clock,8)==ASTER_EINVAL,45);
    /* Host starts with an unprivileged identity, then a different privileged one. */
    int result=aster_fs_open("/data/value",11,ASTER_FS_O_RDONLY,&temp);
    if(result==ASTER_EPERM) {
        CHECK(aster_kv_set("setting",7,"x",1)==ASTER_EPERM,34);
        CHECK(aster_appmgr_count()==ASTER_EPERM && aster_appmgr_start("test.next",9)==ASTER_EPERM,46);
        aster_app_exit(0);return 0;
    }
    CHECK(aster_appmgr_count()==2,47);
    aster_app_info_t app;
    CHECK(aster_appmgr_list(0,&app,sizeof(app))==0 && app.version==2,48);
    CHECK(aster_appmgr_list(0,(void *)0xfffffff0u,sizeof(app))==ASTER_EINVAL,49);
    CHECK(aster_appmgr_start("test.services",13)==ASTER_EINVAL,50);
    CHECK(aster_appmgr_start("test.missing",12)==ASTER_ENOENT,51);
    CHECK(aster_appmgr_uninstall("test.services",13)==ASTER_EINVAL,52);
    if(result==0)aster_fs_close(temp);
    int r=test_files();if(r)return r;
    CHECK(aster_timer_create(0,0,0,&temp)==ASTER_EINVAL,35);
    CHECK(aster_timer_create(2,2,0,&temp)==ASTER_EINVAL,36);
    CHECK(aster_timer_create(2,0,123,&once)==0,37);
    CHECK(aster_timer_create(1,1,456,&repeat)==0,38);
    CHECK(aster_timer_destroy(leaked)==ASTER_EBADF,39);
    return 0;
}
void aster_app_start(void) {}
void aster_app_event(const aster_event_t *event)
{
    if(event->type==ASTER_EVENT_TIMER) {
        if(event->u.timer.id==once && event->u.timer.user==123)ones++;
        else if(event->u.timer.id==repeat && event->u.timer.user==456)repeated++;
        else {aster_app_exit(40);return;}
    }
    if(event->type==ASTER_EVENT_FRAME && ++frames>5 && repeated>1 && ones==1) {
        if(aster_timer_destroy(once)!=0 || aster_timer_destroy(once)!=ASTER_EBADF ||
           aster_timer_destroy(repeat)!=0)aster_app_exit(41);
        else if(aster_appmgr_start("test.next",9)!=0)aster_app_exit(53);
    }
    if(frames>100)aster_app_exit(42);
}
void aster_app_stop(void) {}
void aster_app_destroy(void) {}
