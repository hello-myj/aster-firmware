/* SPDX-License-Identifier: MIT */
#ifndef ASTER_TOUCH_PROTOCOL_H
#define ASTER_TOUCH_PROTOCOL_H

#include "touch_state.h"

#include <stddef.h>

#define ASTER_FT6336_REPORT_BYTES 13u /* 0x02..0x0e, status + two records */
#define ASTER_AXS15231B_REPORT_BYTES 14u /* 2-byte header + two 6-byte records */

int aster_ft6336_decode(const uint8_t *data, size_t len, uint16_t width,
			uint16_t height, struct aster_touch_frame *frame);
int aster_axs15231b_decode(const uint8_t *data, size_t len, uint16_t width,
			   uint16_t height, struct aster_touch_frame *frame);

#endif
