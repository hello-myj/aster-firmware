#!/usr/bin/env python3
"""Run the actual IDF core runner with Linux shims, after tools/host.py builds WAMR.
This verifies ABI/lifecycle/error handling, not FreeRTOS or hardware behavior.
"""
from pathlib import Path
import subprocess
import tempfile
import os
import json
import struct
import hashlib
from concurrent.futures import ThreadPoolExecutor
from make_smoke import ROOT, module
import sys
sys.path.insert(0,str(ROOT/"tools"))
from p4_aot import interruptible_wasm


def main():
    out = ROOT / "build/idf-runtime-check"
    (out / "freertos").mkdir(parents=True, exist_ok=True)
    files = {
        "esp_timer.h": "#include <stdint.h>\n#include <time.h>\nstatic inline int64_t esp_timer_get_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }\n",
        "esp_heap_caps.h": "#include <stdlib.h>\n#include <malloc.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\nstatic inline void *heap_caps_malloc(size_t n, int caps) {(void)caps; return malloc(n);}\nstatic inline void *heap_caps_realloc(void *p,size_t n,int caps) {(void)caps; return realloc(p,n);}\nstatic inline void *heap_caps_aligned_alloc(size_t a,size_t n,int caps) {(void)caps; void *p=NULL; return posix_memalign(&p,a,n)?NULL:p;}\nstatic inline size_t heap_caps_get_allocated_size(void *p) {return malloc_usable_size(p);}\n",
        "freertos/FreeRTOS.h": "typedef unsigned int TickType_t;\n#define pdMS_TO_TICKS(ms) (ms)\n",
        "freertos/task.h": "#include <time.h>\nstatic inline void vTaskDelay(unsigned int ticks) {struct timespec t={ticks/1000,(ticks%1000)*1000000};nanosleep(&t,0);}\n",
        "main.c": '#include "aster_idf_runtime.h"\nint main(int argc,char **argv) { return argc==2 ? aster_idf_run_wasm(argv[1]) : 2; }\n',
    }
    for name, data in files.items():
        (out / name).write_text(("#pragma once\n" if name.endswith(".h") else "")+data)
    component = ROOT / "systems/espidf/components/aster_runtime"
    compile_command = ["cc", "-DWASM_ENABLE_THREAD_MGR=1", "-DWASM_ENABLE_SHARED_MEMORY=1", "-g", "-Wall", "-Wextra", "-Werror", "-I"+str(out),
                    "-I"+str(component), "-I"+str(ROOT / "contract/include"),
                    "-I"+str(ROOT / "modules/wamr/core/iwasm/include"),
                    str(out / "main.c"), str(component / "runtime.c"), str(component / "aot_profile.c"), str(component / "devices.c"), str(component / "graphics.c"), str(component / "timers.c"), str(component / "clock.c"), "-DASTER_IDF_HOST_TEST",
                    str(ROOT / "build/host/libaster_vm.a"), "-lm", "-ldl", "-lpthread",
                    "-o", str(out / "runner")]
    render = os.environ.get("ASTER_TEST_RENDER") == "1"
    if render:
        (out / "sdkconfig.h").write_text("#define CONFIG_ASTER_DISPLAY_WIDTH 480\n#define CONFIG_ASTER_DISPLAY_HEIGHT 480\n#define CONFIG_ASTER_P4_BACKLIGHT_GPIO 20\n")
        (out / "esp_attr.h").write_text("#define IRAM_ATTR\n#define DRAM_ATTR\n")
        (out / "freertos/FreeRTOS.h").write_text("#pragma once\n#include <pthread.h>\n#include <stdint.h>\ntypedef unsigned int TickType_t;\n#define pdMS_TO_TICKS(ms) (ms)\n#define portMAX_DELAY UINT32_MAX\n#define pdTRUE 1\n#define portMUX_TYPE pthread_mutex_t\n#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER\n#define portENTER_CRITICAL(m) pthread_mutex_lock(m)\n#define portEXIT_CRITICAL(m) pthread_mutex_unlock(m)\n")
        (out / "freertos/semphr.h").write_text("#pragma once\n#include <pthread.h>\ntypedef pthread_mutex_t StaticSemaphore_t;\ntypedef pthread_mutex_t *SemaphoreHandle_t;\nstatic inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *s) {pthread_mutexattr_t a;pthread_mutexattr_init(&a);pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE);pthread_mutex_init(s,&a);pthread_mutexattr_destroy(&a);return s;}\nstatic inline int xSemaphoreTakeRecursive(SemaphoreHandle_t m,unsigned t) {return (t?pthread_mutex_lock(m):pthread_mutex_trylock(m))==0;}\nstatic inline int xSemaphoreGiveRecursive(SemaphoreHandle_t m) {return pthread_mutex_unlock(m)==0;}\n")
        render_root=ROOT / "systems/espidf/components/aster_render"
        ui=ROOT / "systems/zephyr/ui"
        img=ROOT / "modules/imgdec"
        lv=ROOT / "third_party/lvgl"
        (out / "esp_log.h").write_text('#include <stdio.h>\n#define ESP_LOG_INFO 3\n#define ESP_LOGI(tag,...) ((void)(tag),fprintf(stderr,__VA_ARGS__))\n#define ESP_LOGW ESP_LOGI\n#define ESP_LOGE ESP_LOGI\n#define ESP_LOGD ESP_LOGI\n')
        heap=out / "esp_heap_caps.h"
        heap.write_text(heap.read_text()+"\n#define MALLOC_CAP_INTERNAL 4\n#define MALLOC_CAP_DMA 8\n")
        includes=[render_root/"compat",ui,ROOT/"systems/zephyr/core",ROOT/"systems/zephyr/services",
                  ROOT/"modules/prettypoly",ROOT/"platform",img,lv,lv/"src"]
        sources=[render_root/"port.c"]+[ROOT/"systems/zephyr/services"/name for name in ("aster_pkg.c","aster_app_mgr.c","aster_versions.c","aster_storage.c","aster_sig.c","aster_kv.c")]+[ui/name for name in ("ui_gfx.c","pp_gfx.c","font_sd.c","ui_font.c","flux_draw.c")]
        sources += [img/name for name in ("imgdec.c","imgdec_png.c","imgdec_jpg.c","vendor/miniz.c","vendor/tjpgd.c")]
        sources += [f for f in (lv/"src").rglob("*.c") if not any(n in f.relative_to(lv/"src").parts for n in ("libs","uefi","rtthread","micropython"))]
        sources += [lv/"src/libs/bin_decoder/lv_bin_decoder.c",lv/"src/libs/rle/lv_rle.c"]
        compile_command += ["-DESP_PLATFORM", "-DASTER_SYSTEM_ESP_IDF", "-DASTER_IDF_HOST_TEST", "-DTEST_RENDER", "-DCONFIG_ASTER_LAUNCHER_FLUX=1", '-DCONFIG_ASTER_TARGET_ID="riscv"', '-DCONFIG_ASTER_AOT_TARGET_ID=2', "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-include",str(render_root/"compat/zephyr/sys/util.h"),
                            '-DLV_CONF_PATH="'+str(ui/"lv_conf.h")+'"', "-DLV_LVGL_H_INCLUDE_SIMPLE",
                            "-DMINIZ_NO_ARCHIVE_APIS", "-DMINIZ_NO_STDIO", "-DMINIZ_NO_TIME",
                            "-Wno-error", "-Wno-unused-parameter"]
        compile_command += ["-I"+str(f) for f in includes]+[str(f) for f in sources]
    if render:
        # Compile the shared renderer once, in parallel; reuse it for both runners.
        all_sources=sources+[component/"runtime.c",component/"aot_profile.c",component/"devices.c",component/"graphics.c",component/"files.c",component/"timers.c",component/"apps.c",component/"clock.c",component/"images.c"]
        flags=[]
        skip=False
        for arg in compile_command[1:]:
            if skip: skip=False;continue
            if arg=="-o": skip=True;continue
            if arg.endswith((".c",".a")) or arg in ("-lm","-ldl","-lpthread","-Wl,--gc-sections"):continue
            flags.append(arg)
        objects=[]
        headers=set()
        for directory in includes+[component,out,ROOT/"contract/include",ROOT/"modules/wamr/core/iwasm/include"]:
            headers.update(directory.rglob("*.h"))
        digest=hashlib.sha256(json.dumps(flags).encode())
        for header in sorted(headers):
            digest.update(str(header).encode());digest.update(header.read_bytes())
        common_digest=digest.digest()
        def compile_source(pair):
            index,source=pair
            obj=out / ("render-%03d.o" % index)
            signature=hashlib.sha256(common_digest+str(source).encode()+source.read_bytes()).hexdigest()
            stamp=obj.with_suffix(".sha256")
            if obj.is_file() and stamp.is_file() and stamp.read_text()==signature:return str(obj)
            subprocess.run(["cc"]+flags+["-c",str(source),"-o",str(obj)],check=True)
            stamp.write_text(signature)
            return str(obj)
        with ThreadPoolExecutor(max_workers=8) as pool:
            objects=list(pool.map(compile_source,enumerate(all_sources)))
        compile_command=["cc"]+flags+[str(out/"main.c")]+objects+[
            str(ROOT/"build/host/libaster_vm.a"),"-lm","-ldl","-lpthread","-Wl,--gc-sections","-o",str(out/"runner")]
    subprocess.run(compile_command, check=True)
    (out/"compile.json").write_text(json.dumps(compile_command))
    compiler = Path(os.environ.get("WASI_CC") or str(ROOT.parent / "aster-sdk/toolchain/wasi-sdk-24.0-x86_64-linux/bin/clang"))
    # Windows checkout stores Git symlinks as text; invoke the ELF binaries directly.
    if compiler.is_file() and compiler.stat().st_size < 128:
        compiler = compiler.parent / compiler.read_text().strip()
    linker_dir = tempfile.TemporaryDirectory(prefix="aster-wasm-linker-")
    linker = Path(linker_dir.name) / "wasm-ld"
    os.symlink(compiler.parent / "lld", linker)
    fixture = Path(__file__).parent
    wasm = out / "graphics.wasm"
    wasm_command = [str(compiler), "-O1", "-nostdlib", "-ffreestanding", "-fno-builtin", "-fuse-ld="+str(linker), "-I"+str(ROOT / "contract/include"),
                    str(fixture / "graphics_app.c"), "-o", str(wasm),
                    "-Wl,--no-entry", "-Wl,--export=aster_app_init", "-Wl,--export=aster_app_start",
                    "-Wl,--export=aster_app_event", "-Wl,--export=aster_app_stop",
                    "-Wl,--export=aster_app_destroy", "-Wl,--export-memory",
                    "-Wl,--export-if-defined=aster_app_pause", "-Wl,--export-if-defined=aster_app_resume",
                    "-Wl,--initial-memory=262144"]
    if render: wasm_command.append("-DTEST_RENDER")
    subprocess.run(wasm_command, check=True)
    demo = ROOT / "build/osptek-p4/sdcard/app.wasm"
    demo.parent.mkdir(parents=True, exist_ok=True)
    demo_command = [str(fixture / "display_demo.c") if arg==str(fixture / "graphics_app.c")
                    else str(demo) if arg==str(wasm) else arg for arg in wasm_command]
    subprocess.run(demo_command, check=True)
    print("Built board display demo:", demo)
    mock_command = [str(fixture / "graphics_mock.c") if arg==str(out / "main.c")
                    else str(out / "graphics-runner") if arg==str(out / "runner") else arg
                    for arg in compile_command]
    subprocess.run(mock_command, check=True)
    subprocess.run([str(out / "graphics-runner"), str(wasm)], check=True)
    scale=out/"scale.wasm"
    scale_command=[str(fixture/"scale_app.c") if arg==str(fixture/"graphics_app.c")
                   else str(scale) if arg==str(wasm) else arg for arg in wasm_command]
    subprocess.run(scale_command,check=True)
    scale_runner=[str(fixture/"scale_mock.c") if arg==str(out/"main.c")
                  else str(out/"scale-runner") if arg==str(out/"runner") else arg for arg in compile_command]
    subprocess.run(scale_runner,check=True)
    subprocess.run([str(out/"scale-runner"),str(scale)],check=True,timeout=8)
    stop_wasm=out/"stop.wasm"
    stop_command=[str(fixture/"stop_app.c") if arg==str(fixture/"graphics_app.c")
                  else str(stop_wasm) if arg==str(wasm) else arg for arg in wasm_command]
    subprocess.run(stop_command,check=True)
    stop_runner=[str(fixture/"stop_mock.c") if arg==str(out/"main.c")
                 else str(out/"stop-runner") if arg==str(out/"runner") else arg for arg in compile_command]
    subprocess.run(stop_runner,check=True)
    subprocess.run([str(out/"stop-runner"),str(stop_wasm)],check=True,timeout=8)
    subprocess.run([str(out/"stop-runner"),str(stop_wasm),"watchdog"],check=True,timeout=8)
    pause_wasm=out/"pause.wasm"
    pause_command=[str(fixture/"pause_app.c") if arg==str(fixture/"graphics_app.c")
                   else str(pause_wasm) if arg==str(wasm) else arg for arg in wasm_command]
    subprocess.run(pause_command,check=True)
    pause_runner=[str(fixture/"pause_mock.c") if arg==str(out/"main.c")
                  else str(out/"pause-runner") if arg==str(out/"runner") else arg for arg in compile_command]
    subprocess.run(pause_runner,check=True)
    subprocess.run([str(out/"pause-runner"),str(pause_wasm)],check=True,timeout=8)
    multi_wasm=out/"multitouch.wasm"
    multi_command=[str(fixture/"multitouch_app.c") if arg==str(fixture/"graphics_app.c")
                   else str(multi_wasm) if arg==str(wasm) else arg for arg in wasm_command]
    subprocess.run(multi_command,check=True)
    multi_runner=[str(fixture/"multitouch_mock.c") if arg==str(out/"main.c")
                  else str(out/"multitouch-runner") if arg==str(out/"runner") else arg for arg in compile_command]
    subprocess.run(multi_runner,check=True)
    subprocess.run([str(out/"multitouch-runner"),str(multi_wasm)],check=True,timeout=8)
    # Native-host AOT exercises the same loader/bridges/termination contract.
    # The separately generated RISC-V image is cross-compiled, never host-run.
    wamrc=ROOT.parent/"aster-sdk/toolchain/wamrc"
    if wamrc.is_file():
        for source in (wasm,stop_wasm,pause_wasm,multi_wasm):
            aot=source.with_suffix(".aot")
            prepared=source.with_suffix(".interruptible.wasm")
            prepared.write_bytes(interruptible_wasm(source.read_bytes()))
            subprocess.run([str(wamrc),"--target=x86_64","--cpu=x86-64","--enable-multi-thread",
                            "--bounds-checks=1","--stack-bounds-checks=1","-o",str(aot),str(prepared)],check=True)
            if source==wasm:subprocess.run([str(out/"graphics-runner"),str(aot)],check=True)
            elif source==multi_wasm:subprocess.run([str(out/"multitouch-runner"),str(aot)],check=True,timeout=8)
            elif source==stop_wasm:
                subprocess.run([str(out/"stop-runner"),str(aot)],check=True,timeout=8)
                subprocess.run([str(out/"stop-runner"),str(aot),"watchdog"],check=True,timeout=8)
            else:subprocess.run([str(out/"pause-runner"),str(aot)],check=True,timeout=8)
        subprocess.run([sys.executable,str(ROOT/"tools/p4_aot.py"),str(demo),str(demo.with_suffix(".aot"))],check=True)
    if render:
        service_wasm=out/"services.wasm"
        service_command=[str(fixture/"services_app.c") if arg==str(fixture/"graphics_app.c")
                         else str(service_wasm) if arg==str(wasm) else arg for arg in wasm_command]
        subprocess.run(service_command,check=True)
        service_runner=[str(fixture/"services_mock.c") if arg==str(out/"main.c")
                        else str(out/"services-runner") if arg==str(out/"runner") else arg for arg in compile_command]
        subprocess.run(service_runner,check=True)
        with tempfile.TemporaryDirectory(prefix="aster-idf-sd-") as sd:
            packages=[]
            for identity in ("test.services","test.next"):
                manifest=json.dumps({"id":identity,"name":identity,"version":"1.0.0","product":"aster",
                    "target_id":"riscv","abi":{"min":"4.2","target":"4.2.0"},"permissions":["fs","kv","appmgr"]}).encode()
                code=service_wasm.read_bytes()
                header=bytearray(64)
                struct.pack_into("<4sHHIIIIIII",header,0,b"ASTP",1,64,0,64,len(manifest),64+len(manifest),len(code),0,0)
                package=Path(sd)/(identity+".ast");package.write_bytes(header+manifest+code);packages.append(str(package))
            subprocess.run([str(out/"services-runner"),str(service_wasm)]+packages,check=True,timeout=20,
                           env={**os.environ,"ASTER_TEST_SD_ROOT":sd})
    (out / "valid.wasm").write_bytes(module())
    (out / "bad.wasm").write_bytes(b"not-a-wasm-module")
    for name, expected in (("valid.wasm",0), ("bad.wasm",1), ("missing.wasm",2)):
        result = subprocess.run([str(out / "runner"), str(out / name)])
        if result.returncode != expected:
            raise RuntimeError(f"{name}: expected {expected}, got {result.returncode}")
        print(f"PASS {name}: {result.returncode}")


if __name__ == "__main__":
    main()
