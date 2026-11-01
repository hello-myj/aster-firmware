/* SPDX-License-Identifier: MIT */
#include <aster/aster.h>
static unsigned seen;
static void check(int ok,int code){if(!ok)aster_app_exit(code);}
int32_t aster_app_init(void) {
    aster_input_info_t info={0};
    check(aster_input_get_info(&info,sizeof(info))==0 && info.max_points==5,60);return 0;
}
void aster_app_start(void) {}
void aster_app_event(const aster_event_t *event) {
    if(event->type!=ASTER_EVENT_TOUCH)return;
    static const unsigned slots[]={0,1,2,3,4, 0,1,2,3,4, 1,3, 0,2,4, 0,2,4, 3,3};
    static const unsigned actions[]={1,1,1,1,1, 2,2,2,2,2, 3,3, 2,2,2, 3,3,3, 1,3};
    if(seen>=20){aster_app_exit(61);return;}
    unsigned slot=slots[seen], n=seen<10?5:seen<15?3:seen<18?0:seen==18?1:0;
    unsigned delta=seen<5 || seen>=18?0:seen<12?1:2;
    check(event->u.touch.slot==slot && event->u.touch.action==actions[seen],62);
    check(event->u.touch.x==10+slot*50+delta && event->u.touch.y==20+slot*50,63);
    aster_touch_point_t points[5];
    check(aster_input_touch((uint32_t)(uintptr_t)points,5)==(int)n,64);
    check(aster_input_touch((uint32_t)(uintptr_t)points,1)==(n?1:0),65);
    if(n)check(aster_input_touch(0xfffffff0u,5)==ASTER_EINVAL,66);
    if(++seen==20)aster_app_exit(0);
}
void aster_app_stop(void){}
void aster_app_destroy(void){}
