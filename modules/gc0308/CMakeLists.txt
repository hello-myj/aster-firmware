# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_VIDEO_GC0308 drivers/video/gc0308.c)
zephyr_library_include_directories_ifdef(CONFIG_VIDEO_GC0308
  ${ZEPHYR_BASE}/drivers/video)
