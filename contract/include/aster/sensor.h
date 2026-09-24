/* SPDX-License-Identifier: MIT
 * ASTER ABI 1.3 - motion sensors (pull model, no events).
 *
 * Gated by capability bits only (ASTER_FEATURE_ACCELEROMETER / _GYROSCOPE);
 * like display/input, sensors are hardware facts, not a permission surface.
 * Values are SI: accel m/s^2 (gravity included, ~9.8 on a still device),
 * gyro rad/s. Read failure on a device without the sensor: ASTER_ENODEV.
 */
#ifndef ASTER_SENSOR_H
#define ASTER_SENSOR_H

#include "types.h"

/* out_xyz: wasm offset of float[3] = {x, y, z}; size must be
 * 3 * sizeof(float). ASTER_EINVAL on bad size / bad pointer. */
ASTER_I("aster_accel_read") int32_t aster_accel_read(float *out_xyz, uint32_t size);
ASTER_I("aster_gyro_read")  int32_t aster_gyro_read(float *out_xyz, uint32_t size);

#endif /* ASTER_SENSOR_H */
