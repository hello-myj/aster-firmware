/* SPDX-License-Identifier: MIT */
#pragma once
#include "sdkconfig.h"

void osptek_storage_start(void);
#if CONFIG_ASTER_OSPTEK_USB_STORAGE
void osptek_usb_storage_register(void);
#endif
