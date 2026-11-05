/* SPDX-License-Identifier: MIT */
#pragma once
int aster_idf_ui_init(int (*launch)(const char *id));
void aster_idf_ui_idle(void);
void aster_idf_ui_before_app(void);
void aster_idf_ui_after_app(void);
/* Console mailbox; all LVGL work and measurements stay on the UI worker. */
int aster_idf_ui_command(int argc,char **argv);

#include "aster_pkg.h"
void aster_idf_ui_install_progress(void *context,aster_install_phase phase,uint64_t done,uint64_t total,const char *file);
void aster_idf_ui_install_end(void);
