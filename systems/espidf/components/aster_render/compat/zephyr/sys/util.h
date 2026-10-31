/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#define ASTER_ENABLED1 _YES,
#define ASTER_IS_ENABLED1(x) ASTER_IS_ENABLED2(ASTER_ENABLED##x)
#define ASTER_IS_ENABLED2(args) ASTER_IS_ENABLED3(args 1,0)
#define ASTER_IS_ENABLED3(ignore,value,...) value
#define IS_ENABLED(x) ASTER_IS_ENABLED1(x)
#define CONFIG_ASTER_GFX_TIMING 0
#define CONFIG_ASTER_LVGL_MEM_KB 256
#define CONFIG_ASTER_IMGDEC_JPG 1
#define CONFIG_ASTER_IMGDEC_PNG 1
#ifndef BUILD_ASSERT
#define BUILD_ASSERT(x,...) _Static_assert(x, #x)
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif
#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif
#ifndef CLAMP
#define CLAMP(x,l,h) MIN(MAX(x,l),h)
#endif
#ifndef ARG_UNUSED
#define ARG_UNUSED(x) (void)(x)
#endif
#ifndef BIT
#define BIT(n) (1UL<<(n))
#endif
#ifndef BIT64
#define BIT64(n) (1ULL<<(n))
#endif
#ifndef ROUND_UP
#define ROUND_UP(x,a) (((x)+(a)-1)/(a)*(a))
#endif

#include <stddef.h>
#define CONTAINER_OF(ptr,type,member) ((type *)((char *)(ptr)-offsetof(type,member)))
