idf_component_register(SRCS "wake.c" "vad.c" "deepseek.c" "deepseek_voice.c" "chat_sse.c" "credentials.c"
    "speech_socket.c" "xfyun_asr.c" "volc_tts.c" "avatar_tool.c" INCLUDE_DIRS "."
    PRIV_REQUIRES espressif__esp-sr espressif__cjson espressif__esp_websocket_client
                  esp_http_client mbedtls nvs_flash esp_timer esp_hw_support tcp_transport)
idf_component_get_property(sr_dir espressif__esp-sr COMPONENT_DIR)
set(model wn9_nihaoxiaozhi)
set(pack_dir "${CMAKE_CURRENT_BINARY_DIR}/voice_models")
set(model_dir "${sr_dir}/model/wakenet_model/${model}")
set(vad_dir "${sr_dir}/model/vadnet_model/vadnet1_medium")
file(GLOB model_files CONFIGURE_DEPENDS "${model_dir}/*" "${vad_dir}/*")
add_custom_command(
    OUTPUT "${pack_dir}/aster_voice_models.bin"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${pack_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${model_dir}" "${pack_dir}/${model}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${vad_dir}" "${pack_dir}/vadnet1_medium"
    COMMAND "${PYTHON}" "${sr_dir}/model/pack_model.py" -m "${pack_dir}" -o aster_voice_models.bin
    DEPENDS ${model_files} "${sr_dir}/model/pack_model.py"
    VERBATIM)
target_add_binary_data(${COMPONENT_LIB} "${pack_dir}/aster_voice_models.bin" BINARY)
set(whale "${CMAKE_CURRENT_BINARY_DIR}/aster_whale.bin")
add_custom_command(OUTPUT "${whale}"
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/whale.py" "${whale}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/whale.py" VERBATIM)
target_add_binary_data(${COMPONENT_LIB} "${whale}" BINARY)
