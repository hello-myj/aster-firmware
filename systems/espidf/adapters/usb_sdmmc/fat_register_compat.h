/* SPDX-License-Identifier: MIT */
#pragma once
#include "esp_vfs_fat.h"

/* Included only in TinyUSB MSC on IDF trees with the legacy declaration. */
#define esp_vfs_fat_register(config, fs) esp_vfs_fat_register_cfg((config), (fs))
