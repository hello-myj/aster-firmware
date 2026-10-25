# SPDX-License-Identifier: Apache-2.0

# A stable ASCII target also supports workspaces whose paths contain Chinese.
zephyr_library_named(aster_gc0308)
zephyr_library_sources_ifdef(CONFIG_VIDEO_GC0308 drivers/video/gc0308.c)
zephyr_library_include_directories_ifdef(CONFIG_VIDEO_GC0308
  ${ZEPHYR_BASE}/drivers/video)
