/* SPDX-License-Identifier: MIT */
#include <aster/aster.h>
static volatile unsigned counter;
int32_t aster_app_init(void) {return 0;}
void aster_app_start(void) {}
void aster_app_event(const aster_event_t *event) {(void)event;for(;;)counter++;}
void aster_app_stop(void) {}
void aster_app_destroy(void) {}
