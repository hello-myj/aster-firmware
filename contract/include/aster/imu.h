/* SPDX-License-Identifier: MIT
 * ABI 3.7: coherent six-axis IMU samples, in the sensor's right-handed axes.
 * Accelerometer includes gravity (m/s^2); angular velocity is rad/s.
 * No permissions: requires accelerometer + gyroscope hardware capabilities.
 */
#ifndef ASTER_IMU_H
#define ASTER_IMU_H
#include "types.h"

#define ASTER_IMU_ACCEL_VALID (1u << 0)
#define ASTER_IMU_GYRO_VALID  (1u << 1)

typedef struct {
    uint32_t size;             /* sizeof(aster_imu_info_t) */
    uint32_t version;          /* 1 */
    uint32_t flags;            /* ASTER_IMU_*_VALID channels available */
    uint32_t min_interval_ms;  /* native fetch cache TTL, not hardware ODR */
} aster_imu_info_t;

typedef struct {
    uint32_t size;             /* sizeof(aster_imu_sample_t) */
    uint32_t version;          /* 1 */
    uint64_t timestamp_ms;     /* monotonic device uptime at fetch completion */
    uint32_t sequence;         /* changes on a new fetch; wraps as u32 */
    uint32_t flags;            /* ASTER_IMU_*_VALID */
    float accel[3];            /* x/y/z, m/s^2, includes gravity */
    float gyro[3];             /* x/y/z, rad/s */
} aster_imu_sample_t;

/* V1 needs a full-size output buffer. OK, EINVAL, ENODEV (absent/not ready),
 * EIO (fetch/channel read failed). Output is written only on success.
 * info also checks that the sensor can be read. No configuration or handles. */
ASTER_I("aster_imu_get_info") int32_t aster_imu_get_info(
    aster_imu_info_t *out, uint32_t out_size);
/* One native fetch supplies all six values. Calls within min_interval_ms
 * may return the same timestamp/sequence; integrate each sequence once.
 * The function performs a short synchronous sensor read, not a background
 * stream. Axes belong to the sensor; display rotation does not remap them. */
ASTER_I("aster_imu_read") int32_t aster_imu_read(
    aster_imu_sample_t *out, uint32_t out_size);
#endif
