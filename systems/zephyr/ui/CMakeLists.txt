# SPDX-License-Identifier: MIT
# Native UI substrate: slim LVGL 9 + fonts + gfx rasterizer + native
# launcher. Flush goes through aster_plat_display_write_at (logical coords;
# the port owns rotation). The retired wasm-side UI ABI (ui.bin widgets)
# lived here as ui_runtime.c/ui_text.c -- drawing apps use the gfx command
# engine (ABI 2.2) instead.

set(ASTER_LVGL_DIR ${ASTER_ROOT}/third_party/lvgl)

file(GLOB_RECURSE ASTER_LVGL_SOURCES CONFIGURE_DEPENDS ${ASTER_LVGL_DIR}/src/*.c)
list(FILTER ASTER_LVGL_SOURCES EXCLUDE REGEX "/(examples|tests|demos)/")
list(FILTER ASTER_LVGL_SOURCES EXCLUDE REGEX "/libs/")
list(FILTER ASTER_LVGL_SOURCES EXCLUDE REGEX "/stdlib/(uefi|rtthread|micropython)/")

# Only the decoders something actually opens: app icons are decoded in
# firmware (launcher_font_gen.py bakes them to ASTI) and handed to LVGL as
# in-memory descriptors, so lv_fs has no consumers left -- and tiny_ttf,
# its only one, is gone with it (aster_lv_fs.c deleted).
list(APPEND ASTER_LVGL_SOURCES
	${ASTER_LVGL_DIR}/src/libs/bin_decoder/lv_bin_decoder.c
	${ASTER_LVGL_DIR}/src/libs/rle/lv_rle.c
)

target_sources(app PRIVATE
	ui_lvgl.c
	ui_font.c
	font_sd.c
	ui_gfx.c
	pp_gfx.c
	ui_splash.c
	# gesture.c has no LVGL in it (press/move/release in, one gesture
	# out). Born in the launcher for page swipes, consumed by qpanel.c
	# once the panel lands, so it belongs to neither and sits at the top.
	gesture.c
	# qpanel.c is NOT under the ASTER_LAUNCHER_NATIVE gate with launcher/:
	# the sheet is a guest of whatever screen is underneath, and from S5c
	# that includes a running app -- which is available whether or not
	# this build has a native launcher at all.
	qpanel.c
	${ASTER_LVGL_SOURCES}
)

if(CONFIG_ASTER_SCREENSHOT)
	target_sources(app PRIVATE ui_screenshot.c)
endif()

if(CONFIG_ASTER_LAUNCHER_NATIVE)
	target_sources(app PRIVATE launcher/launcher.c)
	target_include_directories(app PRIVATE launcher)
endif()

target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${ASTER_LVGL_DIR}
	${ASTER_LVGL_DIR}/src
	${ASTER_ROOT}/modules/prettypoly
)

target_compile_definitions(app PRIVATE
	"LV_CONF_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h\""
	LV_LVGL_H_INCLUDE_SIMPLE
)

# Vendored LVGL: keep upstream style, don't police it. Must use the
# SOURCE + TARGET_DIRECTORY form -- plain set_source_files_properties() does
# not reach the Zephyr `app` target, which is created in another directory
# (same gotcha the imgdec module documents).
set_property(SOURCE ${ASTER_LVGL_SOURCES} TARGET_DIRECTORY app PROPERTY
	COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-unused-variable;-Wno-unused-function;-Wno-sign-compare;-Wno-missing-field-initializers;-Wno-type-limits")
