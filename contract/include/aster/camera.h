/* SPDX-License-Identifier: MIT
 * ASTER ABI 1.3 - camera (single-shot RGB565 still, pull model).
 *
 * Gated by ASTER_FEATURE_CAMERA only — the camera is a hardware fact, not a
 * permission surface (same layering rule as display/sensor). V1 captures one
 * still on demand; there is no continuous stream / preview. aster_cam_read
 * BLOCKS the app worker for the whole capture (sensor power-up + warm-up +
 * one frame; ~1-2 s on the reference port), so call it from a button/timer,
 * NOT from a 30 fps frame callback.
 */
#ifndef ASTER_CAMERA_H
#define ASTER_CAMERA_H

#include "types.h"

/* Native capture geometry, RGB565 (2 bytes/px). out_w/out_h may be NULL.
 * ASTER_ENODEV if the board has no camera. */
ASTER_I("aster_cam_info") int32_t aster_cam_info(uint32_t *out_w, uint32_t *out_h);

/* Capture one RGB565 frame into `buf` (a wasm offset). `size` must be at
 * least w*h*2. Returns ASTER_OK on success; ASTER_ENODEV (no camera),
 * ASTER_EINVAL (buffer too small / bad ptr), or ASTER_EIO (capture failed). */
ASTER_I("aster_cam_read") int32_t aster_cam_read(uint8_t *buf, uint32_t size);

#endif /* ASTER_CAMERA_H */
