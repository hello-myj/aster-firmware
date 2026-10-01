#include <zephyr/sys/util.h>
/* SPDX-License-Identifier: MIT
 * Six-axis IMU for APP imports and shell diagnostics.
 */
#include "aster_platform.h"

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

LOG_MODULE_REGISTER(aster_imu, LOG_LEVEL_INF);

/* Bound by ROLE (`chosen { aster,imu = &<part>; }` in the board overlay), not by
 * node label. The old DT_NODELABEL(qmi8658) made the part number part of the
 * port's interface: a board whose six-axis part is an ICM-42688 could not reuse
 * this file at all, and the failure arrived as an undefined macro rather than
 * as "this board declared no IMU". `aster_imu` is a node-level name and so always
 * EXISTS as a token even when the overlay never sets it -- the assert is what
 * turns that into a build error instead of a device that is silently NULL. */
#if IS_ENABLED(CONFIG_ASTER_HAS_ACCEL) || IS_ENABLED(CONFIG_ASTER_HAS_GYRO)
BUILD_ASSERT(DT_HAS_CHOSEN(aster_imu),
	     "the board conf claims an IMU capability, but the board overlay has "
	     "no `chosen { aster,imu = &<sensor>; }` -- the port would bind no "
	     "device, and `hw imu` would report -ENODEV forever");
#endif

K_MUTEX_DEFINE(imu_lock);

int aster_plat_imu_read(float *ax, float *ay, float *az, float *gx, float *gy, float *gz)
{
	const struct device *imu = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(aster_imu));
	if (!imu || !device_is_ready(imu)) {
		return -ENODEV;
	}
	k_mutex_lock(&imu_lock, K_FOREVER);
	int err = sensor_sample_fetch(imu);
	if (err) {
		k_mutex_unlock(&imu_lock);
		return err;
	}
	struct sensor_value a[3], g[3];
	err = sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, a);
	if (!err) {
		err = sensor_channel_get(imu, SENSOR_CHAN_GYRO_XYZ, g);
	}
	if (err) {
		k_mutex_unlock(&imu_lock);
		return err;
	}
	if (ax) { *ax = sensor_value_to_float(&a[0]); }
	if (ay) { *ay = sensor_value_to_float(&a[1]); }
	if (az) { *az = sensor_value_to_float(&a[2]); }
	if (gx) { *gx = sensor_value_to_float(&g[0]); }
	if (gy) { *gy = sensor_value_to_float(&g[1]); }
	if (gz) { *gz = sensor_value_to_float(&g[2]); }
	k_mutex_unlock(&imu_lock);
	return 0;
}
