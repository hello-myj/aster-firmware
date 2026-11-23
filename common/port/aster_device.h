/* SPDX-License-Identifier: MIT */
#ifndef ASTER_DEVICE_H
#define ASTER_DEVICE_H
#include "aster_platform.h"

/* Firmware-internal interface, independent of the guest APP ABI. Append new
 * operations and bump this version when their layout or semantics change. */
#define ASTER_DEVICE_API_VERSION 2u
typedef struct {
    uint32_t size,version;
    const char *name;
#define ASTER_DEVICE_OP(ret,name,args,call,kind) ret (*name) args;
#include "aster_device_ops.def"
#undef ASTER_DEVICE_OP
} aster_device_ops_t;
typedef enum {
#define ASTER_DEVICE_OP(ret,name,args,call,kind) ASTER_DEVICE_METHOD_##name,
#include "aster_device_ops.def"
#undef ASTER_DEVICE_OP
    ASTER_DEVICE_METHOD_COUNT
} aster_device_method_t;

/* Exactly one backend supplies this constant; normal boot allocates no table. */
extern const aster_device_ops_t aster_device_backend;
const aster_device_ops_t *aster_device_ops(void);
bool aster_device_has(aster_device_method_t method);
/* Boot-only registration: table and name must outlive all service tasks.
 * seal() makes subsequent reconfiguration fail, preventing hot-swap races. */
int aster_device_bind(const aster_device_ops_t *ops);
void aster_device_seal(void);
/* C equivalent of inheriting a backend and overriding selected methods.
 * NULL override entries retain the base implementation. To remove a method,
 * derive first, then explicitly clear that member before binding. */
int aster_device_derive(aster_device_ops_t *out,const aster_device_ops_t *base,
                        const aster_device_ops_t *overrides);

#define ASTER_DEVICE_OP(ret,name,args,call,kind) ret aster_device_##name args;
#include "aster_device_ops.def"
#undef ASTER_DEVICE_OP
#endif
