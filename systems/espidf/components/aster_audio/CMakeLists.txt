set(AUDIO "${CMAKE_CURRENT_LIST_DIR}/../../../../platform/esp32s3")
idf_component_register(SRCS "port.c" "${AUDIO}/audio.c" "${AUDIO}/audio_effect_engine.c"
    "${AUDIO}/audio_file_reader.c" "${AUDIO}/audio_lyrics.c" "${AUDIO}/minimp3_impl.c"
    INCLUDE_DIRS "." "${AUDIO}" "../../../../modules/minimp3"
    REQUIRES aster_render esp_driver_i2c esp_driver_i2s esp_driver_gpio freertos)
target_compile_definitions(${COMPONENT_LIB} PUBLIC CONFIG_ASTER_AUDIO=1 CONFIG_ASTER_AUDIO_ADC_ES8311=1
    CONFIG_ASTER_AUDIO_RATE=16000 CONFIG_ASTER_AUDIO_CHANNELS=2 CONFIG_ASTER_AUDIO_BITS=16)
