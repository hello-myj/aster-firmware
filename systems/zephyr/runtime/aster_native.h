/* SPDX-License-Identifier: MIT */
#ifndef ASTER_NATIVE_H
#define ASTER_NATIVE_H

#include <stdint.h>
#include "wasm_export.h"

NativeSymbol *aster_native_symbols(void);
uint32_t aster_native_symbol_count(void);

/* defined in aster_runtime.c */
typedef struct aster_app_ctx aster_app_ctx_t;
void aster_runtime_terminate(void);

/* defined in aster_native.c: close host fs handles a stopping app left open */
void aster_native_release_files(aster_app_ctx_t *ctx);
void aster_native_frame_refresh(aster_app_ctx_t *ctx);
void aster_native_frame_abort(aster_app_ctx_t *ctx);
void aster_native_frame_release(aster_app_ctx_t *ctx);

void aster_native_release_network(aster_app_ctx_t *ctx);

#endif
