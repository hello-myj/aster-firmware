/* SPDX-License-Identifier: MIT */
#pragma once
int aster_idf_ui_init(int (*launch)(const char *id));
void aster_idf_ui_idle(void);
void aster_idf_ui_before_app(void);
void aster_idf_ui_after_app(void);
