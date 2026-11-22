"""Exercise the real component declarations with an out-of-tree board."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cmake"), "CMake required")
class BoardAdapters(unittest.TestCase):
    def test_new_board_supplies_wiring_without_hosted_or_bluetooth(self):
        with tempfile.TemporaryDirectory() as directory:
            board = Path(directory)
            (board / "aster_board.cmake").write_text(
                'set(ASTER_BOARD_AUDIO_ADAPTER "${ASTER_IDF_ADAPTERS}/es8311_i2s/adapter.cmake")\n'
                'set(ASTER_BOARD_AUDIO_SOURCES "${CMAKE_CURRENT_LIST_DIR}/wiring.c")\n'
                'set(ASTER_BOARD_MEDIA_PLAYER ON)\n'
            )
            (board / "wiring.c").touch()
            fixture = board / "check.cmake"
            fixture.write_text('''cmake_minimum_required(VERSION 3.20)
function(idf_build_get_property out key)
    if(NOT key STREQUAL "PROJECT_DIR")
        message(FATAL_ERROR "unexpected IDF query: ${key}")
    endif()
    set(${out} "${board}" PARENT_SCOPE)
endfunction()
function(idf_component_register)
    set_property(GLOBAL PROPERTY "${component}_args" "${ARGV}")
endfunction()
function(target_compile_definitions target)
    set_property(GLOBAL APPEND PROPERTY "${component}_defs" ${ARGN})
endfunction()
function(load_component component)
    set(COMPONENT_LIB "${component}")
    include("${root}/systems/espidf/components/${component}/CMakeLists.txt")
endfunction()
load_component(aster_audio)
load_component(aster_connectivity)
get_property(audio GLOBAL PROPERTY aster_audio_args)
get_property(defs GLOBAL PROPERTY aster_audio_defs)
get_property(wifi GLOBAL PROPERTY aster_connectivity_args)
if(NOT "${board}/wiring.c" IN_LIST audio OR NOT "player.c" IN_LIST audio
   OR NOT "${root}/systems/espidf/adapters/es8311_i2s/port.c" IN_LIST audio
   OR NOT "ASTER_IDF_MEDIA_PLAYER=1" IN_LIST defs
   OR NOT "ASTER_I2S_MEDIA_PCM=1" IN_LIST defs OR "bt" IN_LIST audio)
    message(FATAL_ERROR "unexpected audio registration: ${audio} / ${defs}")
endif()
if(NOT "esp_wifi" IN_LIST wifi OR "esp_hosted" IN_LIST wifi)
    message(FATAL_ERROR "unexpected Wi-Fi registration: ${wifi}")
endif()
''', encoding="utf-8")
            subprocess.run(["cmake", "-Dboard=" + board.as_posix(),
                            "-Droot=" + ROOT.as_posix(), "-P", str(fixture)], check=True)
