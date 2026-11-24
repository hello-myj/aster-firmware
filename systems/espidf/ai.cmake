# Include before project.cmake so builds without AI also omit its model dependencies.
option(ASTER_AI "Include the voice companion, offline wake word and DeepSeek speech services" OFF)
if(ASTER_AI)
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/optional/aster_ai")
endif()

function(aster_ai_configure_build)
    if(ASTER_AI)
        idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.ai.lock")
        idf_build_set_property(ASTER_AI_ENABLED ON)
    endif()
endfunction()
