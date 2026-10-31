/* SPDX-License-Identifier: MIT. Hardware-neutral lifecycle/ABI fixture. */
#include <aster/aster.h>
int32_t aster_app_init(void) {return aster_system_get_abi_version()==ASTER_ABI_VERSION?0:ASTER_EABI;}
void aster_app_start(void) {}
void aster_app_event(const aster_event_t *event)
{(void)event;aster_log(0,"application lifecycle passed\n",29);aster_app_exit(0);}
void aster_app_stop(void) {}
void aster_app_destroy(void) {}
