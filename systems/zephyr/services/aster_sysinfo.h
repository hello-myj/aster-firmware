/* SPDX-License-Identifier: MIT
 * Device capability model (design doc sec.7 / sec.23).
 */
#ifndef ASTER_SYSINFO_H
#define ASTER_SYSINFO_H

#include <stdint.h>
#include <stdbool.h>
#include <aster/system.h>

/* Device profile id / platform / soc are provided by the board port through
 * Kconfig (CONFIG_ASTER_BOARD_ID / _PLATFORM / _SOC), so this header and the
 * system layer carry no board-specific literals. */
#ifndef ASTER_SYSVER
#define ASTER_SYSVER     "0.1.0"
#endif

void aster_sysinfo_get(aster_system_info_t *info);
bool aster_sysinfo_has_feature(uint32_t bit);

/* Printable name for a cpu_arch id (ASTER_ARCH_*), for shell/diagnostics. */
static inline const char *aster_arch_name(uint32_t arch)
{
	switch (arch) {
	case ASTER_ARCH_XTENSA: return "xtensa";
	case ASTER_ARCH_ARM:    return "arm";
	case ASTER_ARCH_RISCV:  return "riscv";
	case ASTER_ARCH_X86:    return "x86";
	default:              return "unknown";
	}
}

/* Capability name from manifest requires/optional -> bit (design doc sec.8) */
bool aster_sysinfo_feature_by_name(const char *name);

#endif
