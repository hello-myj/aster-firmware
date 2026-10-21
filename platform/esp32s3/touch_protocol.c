/* SPDX-License-Identifier: MIT
 * Pure protocol decoders. Kept separate from Zephyr I2C so the exact
 * production parser can run in host tests and sanitizers.
 */
#include "touch_protocol.h"

#include <errno.h>

static int decode_record(const uint8_t *p, uint8_t id, uint8_t event,
			uint16_t width, uint16_t height,
			struct aster_touch_sample *out)
{
	uint16_t x = (uint16_t)(((uint16_t)(p[0] & 0x0f) << 8) | p[1]);
	uint16_t y = (uint16_t)(((uint16_t)(p[2] & 0x0f) << 8) | p[3]);
	uint8_t state;

	if (event == 0u) state = ASTER_TOUCH_SAMPLE_ACTIVE;       /* down */
	else if (event == 1u) state = ASTER_TOUCH_SAMPLE_RELEASE; /* up */
	else if (event == 2u) state = ASTER_TOUCH_SAMPLE_ACTIVE;  /* contact */
	else state = ASTER_TOUCH_SAMPLE_NONE;

	if (state == ASTER_TOUCH_SAMPLE_ACTIVE &&
	    (!width || !height || x >= width || y >= height)) return -ERANGE;
	out->id = id;
	out->state = state;
	out->fresh_down = event == 0u;
	out->x = x;
	out->y = y;
	return 0;
}

int aster_ft6336_decode(const uint8_t *data, size_t len, uint16_t width,
			uint16_t height, struct aster_touch_frame *frame)
{
	if (!data || !frame || len != ASTER_FT6336_REPORT_BYTES || !width || !height)
		return -EINVAL;
	uint8_t count = data[0] & 0x0f;
	if ((data[0] & 0xf0) != 0 || count > ASTER_TOUCH_SLOT_COUNT) return -EINVAL;
	struct aster_touch_frame decoded = { .count = count };
	uint16_t ids = 0;
	for (uint8_t i = 0; i < count; i++) {
		const uint8_t *p = &data[1u + (size_t)i * 6u];
		uint8_t id = p[2] >> 4;
		uint8_t event = p[0] >> 6;
		/* FocalTech reserves ID 0xf for an empty record. */
		if (id == 0x0f || (ids & (1u << id))) return -EINVAL;
		ids |= (uint16_t)(1u << id);
		int rc = decode_record(p, id, event, width, height, &decoded.points[i]);
		if (rc) return rc;
		/* Zephyr's legacy FT5336 driver reports (controller Y, controller X)
		 * and the board calibration/rotation table is built for that native
		 * transposed space. Preserve that mapping in the custom driver. */
		uint16_t raw_x = decoded.points[i].x;
		decoded.points[i].x = decoded.points[i].y;
		decoded.points[i].y = raw_x;
		if (decoded.points[i].state == ASTER_TOUCH_SAMPLE_ACTIVE &&
		    (decoded.points[i].x >= height || decoded.points[i].y >= width))
			return -ERANGE;
	}
	*frame = decoded;
	return 0;
}

int aster_axs15231b_decode(const uint8_t *data, size_t len, uint16_t width,
			   uint16_t height, struct aster_touch_frame *frame)
{
	if (!data || !frame || len != ASTER_AXS15231B_REPORT_BYTES || !width || !height)
		return -EINVAL;
	uint8_t count = data[1];
	if (count > ASTER_TOUCH_SLOT_COUNT || (count > 0 && data[0] == 0xff))
		return -EINVAL;
	struct aster_touch_frame decoded = { .count = count };
	uint16_t ids = 0;
	for (uint8_t i = 0; i < count; i++) {
		const uint8_t *p = &data[2u + (size_t)i * 6u];
		uint8_t id = p[2] >> 4;
		uint8_t event = p[0] >> 6;
		/* Available vendor examples disagree on this field: ARCS names record
		 * byte 4 as ID_POS, while Waveshare's BSP masks the high nibble as
		 * reserved and consumes only Y[11:8]. Retain the ARCS high-nibble ID
		 * mapping rather than record order. The Waveshare hardware capture
		 * reproduced in test_touch_multitouch.py confirms distinct IDs 0/1
		 * in this nibble; crossing/record-reordering acceptance is separate. */
		if (ids & (1u << id)) return -EINVAL;
		ids |= (uint16_t)(1u << id);
		int rc = decode_record(p, id, event, width, height, &decoded.points[i]);
		if (rc) return rc;
	}
	*frame = decoded;
	return 0;
}
