# Include after idf_component_register in boards that expose USB MSC.
target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/usb_sdmmc.c")
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/../../../../common/ui")
# The MSC implementation also defines a weak START STOP callback in its own
# translation unit. Rename that definition so the host eject reaches ours.
idf_component_get_property(esp_tinyusb_lib espressif__esp_tinyusb COMPONENT_LIB)
target_compile_definitions(${esp_tinyusb_lib} PRIVATE
    tud_msc_start_stop_cb=aster_unused_esp_tinyusb_start_stop_cb)

# Some IDF 6.0 trees still export the two-argument FAT registration as
# esp_vfs_fat_register_cfg. Match the installed header, not the version tag.
if(IDF_VERSION_MAJOR EQUAL 6 AND IDF_VERSION_MINOR EQUAL 0)
    idf_component_get_property(fatfs_dir fatfs COMPONENT_DIR)
    file(STRINGS "${fatfs_dir}/vfs/esp_vfs_fat.h" legacy_register
        REGEX "esp_vfs_fat_register[(]const char[*]")
    if(legacy_register)
        idf_component_get_property(tinyusb_dir espressif__esp_tinyusb COMPONENT_DIR)
        set_property(SOURCE "${tinyusb_dir}/tinyusb_msc.c" TARGET_DIRECTORY ${esp_tinyusb_lib}
            APPEND PROPERTY COMPILE_OPTIONS "-include${CMAKE_CURRENT_LIST_DIR}/fat_register_compat.h")
    endif()
endif()
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=tud_msc_read10_cb,--wrap=tud_msc_write10_cb,--wrap=tud_msc_test_unit_ready_cb,--wrap=tud_msc_scsi_cb")
