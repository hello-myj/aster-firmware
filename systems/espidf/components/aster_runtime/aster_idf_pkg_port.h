/* SPDX-License-Identifier: MIT */
#pragma once
#include "aster_mem.h"
#define k_malloc aster_psram_alloc
#define k_free aster_psram_free
bool aster_sysinfo_feature_by_name(const char *name);
