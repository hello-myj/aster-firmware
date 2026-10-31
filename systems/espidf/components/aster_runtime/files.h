/* SPDX-License-Identifier: MIT */
#pragma once
#include <wasm_export.h>
#include <stdint.h>
#include <stdbool.h>
bool aster_idf_has_permission(uint32_t permission);
const char *aster_idf_app_identity(void);
void aster_idf_files_context(const char *id,uint32_t permissions);
void aster_idf_files_cleanup(void);
int aster_idf_resolve_path(wasm_exec_env_t env,uint32_t off,uint32_t len,bool write,char out[256]);
NativeSymbol *aster_idf_file_symbols(uint32_t *count);
