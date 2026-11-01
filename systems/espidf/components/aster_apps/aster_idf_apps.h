/* SPDX-License-Identifier: MIT */
#pragma once
int aster_idf_apps_start(void);
int aster_idf_apps_command(int argc,char **argv);
int aster_idf_apps_launch(const char *id);

/* Register before apps_start; callback runs on the sole display owner. */
void aster_idf_apps_set_color_test(int (*test)(void));
