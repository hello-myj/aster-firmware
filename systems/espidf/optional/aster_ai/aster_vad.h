/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

/* Owned by the recording task after the wake worker releases ESP-SR. */
int aster_vad_open(unsigned silence_ms); /* samples per 16 kHz mono frame, or negative errno */
int aster_vad_detect(int16_t *frame); /* 1 speech, 0 silence, or negative errno */
void aster_vad_close(void);
