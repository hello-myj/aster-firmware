/* SPDX-License-Identifier: MIT */
#include <aster/aster.h>
static unsigned frames,pauses,resumes,pause_frame;
int32_t aster_app_init(void) {return aster_app_set_framerate(10);}
void aster_app_start(void) {}
void aster_app_pause(void) {pauses++;pause_frame=frames;}
void aster_app_resume(void) {resumes++;if(frames!=pause_frame)aster_app_exit(11);}
void aster_app_event(const aster_event_t *event)
{
    if(event->type!=ASTER_EVENT_FRAME)return;
    frames++;
    if(pauses==1 && resumes==1)aster_app_exit(0);
    if(frames>40)aster_app_exit(12);
}
void aster_app_stop(void) {}
void aster_app_destroy(void) {}
