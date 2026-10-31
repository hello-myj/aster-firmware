/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
int aster_idf_audio_start(void);
bool aster_idf_audio_available(void);
void aster_idf_audio_cancel_record(bool cancel);
bool aster_idf_audio_record_cancelled(void);
