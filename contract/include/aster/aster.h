/* SPDX-License-Identifier: MIT
 * ASTER ABI umbrella header (single source of truth).
 *
 * Version constants live in system.h; this header only wires the surface.
 * Apps include <aster/aster.h> only. Never include Zephyr/LVGL/vendor headers.
 */
#ifndef ASTER_ASTER_H
#define ASTER_ASTER_H

#include <stdint.h>
#include <stddef.h>

#include "error.h"
#include "types.h"
#include "log.h"
#include "system.h"
#include "event.h"
#include "app.h"
#include "timer.h"
#include "display.h"
#include "surface.h"
#include "input.h"
#include "fs.h"
#include "kv.h"
#include "appmgr.h"
#include "sensor.h"
#include "camera.h"
#include "audio.h"
#include "time.h"
#include "wifi.h"
#include "img.h"
#include "gfx.h"

#endif /* ASTER_ASTER_H */
