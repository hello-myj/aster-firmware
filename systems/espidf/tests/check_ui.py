#!/usr/bin/env python3
"""Render the native desktop on Linux. Radio, codec, LEDC and panel are mocks."""
from pathlib import Path
import json
import os
import subprocess
import tempfile
import struct
from concurrent.futures import ThreadPoolExecutor
from make_smoke import ROOT


def main():
    out=ROOT/"build/idf-runtime-check"
    command=json.loads((out/"compile.json").read_text())
    ui=ROOT/"systems/espidf/components/aster_ui"
    shared=ROOT/"systems/zephyr/ui"
    fixture=Path(__file__).parent
    (out/"driver").mkdir(exist_ok=True)
    (out/"esp_netif.h").write_text("#pragma once\ntypedef struct esp_netif esp_netif_t;\n")
    (out/"esp_event.h").write_text("#pragma once\ntypedef const char *esp_event_base_t;\n")
    (out/"driver/ledc.h").write_text('''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_TIMER_8_BIT 8
#define LEDC_TIMER_1 1
#define LEDC_AUTO_CLK 0
#define LEDC_CHANNEL_0 0
typedef struct {int speed_mode,duty_resolution,timer_num,freq_hz,clk_cfg;} ledc_timer_config_t;
typedef struct {int gpio_num,speed_mode,channel,timer_sel,duty;} ledc_channel_config_t;
static inline int ledc_timer_config(const ledc_timer_config_t *c) {(void)c;return 0;}
static inline int ledc_channel_config(const ledc_channel_config_t *c) {(void)c;return 0;}
static inline int ledc_set_duty(int m,int c,int d) {(void)m;(void)c;(void)d;return 0;}
static inline int ledc_update_duty(int m,int c) {(void)m;(void)c;return 0;}
''')
    flags=[]
    skip=False
    for arg in command[1:]:
        if skip:skip=False;continue
        if arg=="-o":skip=True;continue
        if arg.endswith((".c",".o",".a")) or arg in ("-lm","-ldl","-lpthread","-Wl,--gc-sections"):continue
        flags.append(arg)
    flags += ["-I"+str(ui),"-I"+str(shared/"launcher"),"-I"+str(ROOT/"platform/esp32s3"),
              "-I"+str(ROOT/"systems/espidf/components/aster_connectivity"),
              "-DCONFIG_ASTER_WIFI=1","-DCONFIG_ASTER_AUDIO=1","-DCONFIG_ASTER_AUDIO_RATE=16000","-DCONFIG_ASTER_FLUX_ART_CACHE=8",
              "-DCONFIG_ASTER_AUDIO_BITS=16",'-DCONFIG_ASTER_BOARD_ID="osptek-p4-st7102"']
    if os.environ.get("ASTER_TEST_RGB888")=="1":flags.append("-DCONFIG_ASTER_P4_RGB666_OUTPUT=1")
    sources=[ui/name for name in ("ui.c","platform.c","wifi.c")]
    sources += [shared/name for name in ("launcher/flux.c","launcher/flux_font.c","gesture.c","qpanel.c","wifi_settings.c","ui_splash.c")]
    sources += [ROOT/"platform/esp32s3/touch_ui.c",fixture/"ui_mock.c",fixture/"flux_pixels.c"]
    def compile_source(pair):
        i,source=pair;obj=out/("ui-%02d.o"%i)
        subprocess.run(["cc"]+flags+["-c",str(source),"-o",str(obj)],check=True)
        return str(obj)
    with ThreadPoolExecutor(max_workers=8) as pool:
        objects=list(pool.map(compile_source,enumerate(sources)))
    link=[arg for arg in command if arg!=str(out/"main.c")]
    link=[str(out/"ui-runner") if arg==str(out/"runner") else arg for arg in link]
    subprocess.run(link+objects,check=True)
    compiler=ROOT.parent/"aster-sdk/toolchain/wasi-sdk-24.0-x86_64-linux/bin/clang-18"
    guest=out/"ui-guest.wasm"
    with tempfile.TemporaryDirectory(prefix="aster-ui-linker-") as directory:
        linker=Path(directory)/"wasm-ld";linker.symlink_to(compiler.parent/"lld")
        subprocess.run([str(compiler),"-O1","-nostdlib","-ffreestanding","-fno-builtin","-fuse-ld="+str(linker),
                        "-I"+str(ROOT/"contract/include"),str(fixture/"ui_guest.c"),"-o",str(guest),"-Wl,--no-entry",
                        *["-Wl,--export=aster_app_"+name for name in ("init","start","event","stop","destroy")]],check=True)
    with tempfile.TemporaryDirectory(prefix="aster-ui-sd-") as sd:
        fonts=Path(sd)/"aster/fonts";fonts.parent.mkdir();fonts.symlink_to(ROOT/"resources/fonts")
        for i in range(9):
            manifest=json.dumps({"id":f"test.ui.{i}","name":f"Test Card {i}","version":"1.0.0","product":"aster",
                "target_id":"riscv","abi":{"min":"4.2","target":"4.2.0"},"permissions":[]}).encode()
            code=guest.read_bytes();header=bytearray(64)
            struct.pack_into("<4sHHIIIIIII",header,0,b"ASTP",1,64,0,64,len(manifest),64+len(manifest),len(code),0,0)
            (Path(sd)/f"test.ui.{i}.ast").write_bytes(header+manifest+code)
        subprocess.run([str(out/"ui-runner"),str(out),str(guest)],check=True,timeout=25,
                       env={**os.environ,"ASTER_TEST_SD_ROOT":sd})
    try:
        from PIL import Image
    except ImportError:
        print("UI previews saved as PPM (Pillow not installed)")
    else:
        for name in ("desktop","desktop-panel","app-panel"):
            Image.open(out/(name+".ppm")).save(out/(name+".png"))


if __name__=="__main__":main()
