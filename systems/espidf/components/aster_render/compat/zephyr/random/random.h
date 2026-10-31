/* SPDX-License-Identifier: MIT */
#pragma once
#include "esp_random.h"
#define sys_rand_get esp_fill_random
int aster_idf_secure_random(void *out,size_t size);
#define sys_csrand_get aster_idf_secure_random
