/* SPDX-License-Identifier: MIT */
#ifndef ASTER_PKG_H
#define ASTER_PKG_H
#include "aster_pkg_format.h"

/* Backend entry point: binds allocation, device capabilities and logging. */
int aster_pkg_parse(const uint8_t *img, size_t size, aster_manifest_t *m,
                   const uint8_t **wasm, size_t *wasm_len,
                   const uint8_t **aot, size_t *aot_len, uint32_t *aot_target);

/* Install a package file into /SD:/aster/apps/<id>. Validates ABI + caps. */
int aster_pkg_install_file(const char *src_path);
int aster_pkg_install_bundle_file(const char *src_path,const char *resources,const char *caller);

#ifdef CONFIG_ASTER_PKG_FAULT_INJECTION
int aster_pkg_fault_arm(const char *id, const char *stage, const char *operation,
                        unsigned action);
void aster_pkg_fault_clear(void);
#endif


#endif
