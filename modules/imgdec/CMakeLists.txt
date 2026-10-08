# SPDX-License-Identifier: MIT
# imgdec: streaming BMP + JPEG (TJpgDec) + PNG (miniz) image decoder front-end.
# Compiled into the app when CONFIG_ASTER_IMGDEC is enabled (see firmware/CMakeLists.txt).
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
target_sources(app PRIVATE imgdec.c)

# NOTE: neither set_source_files_properties() nor add_compile_definitions()
# reach the Zephyr `app` target (created in another dir); the SOURCE
# property + TARGET_DIRECTORY form (CMake >= 3.18) does.
if(CONFIG_ASTER_IMGDEC_JPG)
	target_sources(app PRIVATE imgdec_jpg.c vendor/tjpgd.c)
	# third-party decoder: keep its upstream style, don't police warnings
	set_property(SOURCE vendor/tjpgd.c TARGET_DIRECTORY app PROPERTY
		COMPILE_OPTIONS "-Wno-shadow;-Wno-unused-variable")
endif()

if(CONFIG_ASTER_IMGDEC_PNG)
	target_sources(app PRIVATE imgdec_png.c vendor/miniz.c)
	# Deflate stays: the screenshot path compresses an existing frame with it.
	# Zip and stdio stay out.
	set_property(SOURCE vendor/miniz.c TARGET_DIRECTORY app PROPERTY
		COMPILE_OPTIONS "-DMINIZ_NO_ARCHIVE_APIS;-DMINIZ_NO_STDIO;-DMINIZ_NO_TIME;-Wno-shadow;-Wno-unused-variable")
endif()
