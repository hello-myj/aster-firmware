/* SPDX-License-Identifier: MIT: private callback deadline policy. */
#ifndef ASTER_WATCHDOG_H
#define ASTER_WATCHDOG_H
#include <stdint.h>
enum aster_callback_phase {ASTER_CB_NONE,ASTER_CB_INIT,ASTER_CB_START,ASTER_CB_EVENT,
                          ASTER_CB_PAUSE,ASTER_CB_RESUME,ASTER_CB_STOP,ASTER_CB_DESTROY};
int aster_watchdog_init(void);
void aster_watchdog_begin(const char *id,enum aster_callback_phase phase);
/* true: callback returned after its deadline; recover normally as a failure. */
int aster_watchdog_end(void);
#endif
