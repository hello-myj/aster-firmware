/* SPDX-License-Identifier: MIT */
#ifndef ASTER_TIME_SERVICE_H
#define ASTER_TIME_SERVICE_H

#include <aster/time.h>

/* Internal firmware read; SNTP control remains private to this service. */
int aster_time_read(aster_time_info_t *out);

#endif /* ASTER_TIME_SERVICE_H */
