/* SPDX-License-Identifier: MIT */
#pragma once
#include "aster_power.h"
void aster_power_ui_update(const aster_power_request_t *request,unsigned stage,int result);
bool aster_power_ui_usb_tick(bool connected, const char *connection_hint);
