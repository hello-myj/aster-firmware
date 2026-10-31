/* SPDX-License-Identifier: MIT */
#include <wasm_export.h>
#include <aster/error.h>
/* These optional devices are not fitted/configured in this board profile.
 * Keep the ABI imports resolvable so apps can query capabilities and choose
 * their fallback path. No measurements or successful captures are fabricated. */
static int32_t absent(wasm_exec_env_t env,uint32_t out,uint32_t size)
{(void)env;(void)out;(void)size;return ASTER_ENODEV;}
static int32_t keys(wasm_exec_env_t env,uint32_t out,uint32_t max)
{(void)env;(void)out;(void)max;return 0;}
static NativeSymbol symbols[]={
 {"aster_accel_read",(void *)absent,"(ii)i",NULL},{"aster_gyro_read",(void *)absent,"(ii)i",NULL},
 {"aster_imu_get_info",(void *)absent,"(ii)i",NULL},{"aster_imu_read",(void *)absent,"(ii)i",NULL},
 {"aster_cam_info",(void *)absent,"(ii)i",NULL},{"aster_cam_read",(void *)absent,"(ii)i",NULL},
 {"aster_input_keys_state",(void *)keys,"(ii)i",NULL},
};
NativeSymbol *aster_idf_device_symbols(uint32_t *count) {*count=sizeof(symbols)/sizeof(symbols[0]);return symbols;}
