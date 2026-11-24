/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

/* One inference worker owns a session from open through close. */
int aster_wake_open(void); /* samples per 16 kHz mono frame, or a negative errno */
int aster_wake_detect(int16_t *frame);
void aster_wake_close(void);
