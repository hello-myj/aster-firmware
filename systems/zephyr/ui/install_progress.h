/* SPDX-License-Identifier: MIT */
#ifndef ASTER_INSTALL_PROGRESS_H
#define ASTER_INSTALL_PROGRESS_H
#include "aster_pkg.h"
/* Installer/UI worker only. Does not launch apps or take the registry lock. */
void aster_ui_install_progress(void *context,aster_install_phase phase,
                               uint64_t done,uint64_t total,const char *file);
void aster_ui_install_end(void);
#endif
