/* SPDX-License-Identifier: MIT
 * Development host: the core ABI and lifecycle, with no advertised peripherals.
 * Full device services remain in the Zephyr backend. No SDK sources are linked.
 */
#include <aster/aster.h>
#include <wasm_export.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int exited, exit_code;
static uint32_t fps = 60;

static uint32_t now_ms(wasm_exec_env_t env)
{
    (void)env;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

static uint32_t abi_version(wasm_exec_env_t env)
{
    (void)env;
    return ASTER_ABI_VERSION;
}

static int32_t has_feature(wasm_exec_env_t env, uint32_t bit)
{
    (void)env; (void)bit;
    return 0;
}

static int32_t display_count(wasm_exec_env_t env)
{
    (void)env;
    return 0;
}

static void app_exit(wasm_exec_env_t env, int32_t code)
{
    (void)env;
    exited = 1;
    exit_code = code;
}

static int32_t set_framerate(wasm_exec_env_t env, uint32_t rate)
{
    (void)env;
    if (rate == 0 || rate > 120) return ASTER_EINVAL;
    fps = rate;
    return ASTER_OK;
}

static void app_log(wasm_exec_env_t env, uint32_t level, uint32_t offset, uint32_t size)
{
    (void)level;
    wasm_module_inst_t instance = wasm_runtime_get_module_inst(env);
    if (!wasm_runtime_validate_app_addr(instance, offset, size)) return;
    fwrite(wasm_runtime_addr_app_to_native(instance, offset), 1, size, stdout);
    fflush(stdout);
}

static NativeSymbol symbols[] = {
    {"aster_system_get_abi_version", (void *)abi_version, "()i", NULL},
    {"aster_system_has_feature", (void *)has_feature, "(i)i", NULL},
    {"aster_log", (void *)app_log, "(iii)", NULL},
    {"aster_time_now_ms", (void *)now_ms, "()i", NULL},
    {"aster_app_exit", (void *)app_exit, "(i)", NULL},
    {"aster_app_set_framerate", (void *)set_framerate, "(i)i", NULL},
    {"aster_display_count", (void *)display_count, "()i", NULL},
};

static int call(wasm_module_inst_t instance, wasm_exec_env_t env,
                const char *name, uint32_t count, uint32_t *args)
{
    wasm_function_inst_t function = wasm_runtime_lookup_function(instance, name);
    if (!function) {
        fprintf(stderr, "missing lifecycle export: %s\n", name);
        return 0;
    }
    if (!wasm_runtime_call_wasm(env, function, count, args)) {
        fprintf(stderr, "%s: %s\n", name, wasm_runtime_get_exception(instance));
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: aster-host app.wasm\n"); return 2; }
    FILE *file = fopen(argv[1], "rb");
    if (!file) { perror(argv[1]); return 2; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 2; }
    long size = ftell(file);
    if (size < 8 || size > 16 * 1024 * 1024) { fclose(file); return 2; }
    rewind(file);
    uint8_t *bytes = malloc((size_t)size);
    if (!bytes || fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        fclose(file); free(bytes); return 2;
    }
    fclose(file);
    RuntimeInitArgs init = {0};
    init.mem_alloc_type = Alloc_With_System_Allocator;
    init.native_module_name = "env";
    init.native_symbols = symbols;
    init.n_native_symbols = sizeof(symbols) / sizeof(symbols[0]);
    if (!wasm_runtime_full_init(&init)) { free(bytes); return 2; }
    char error[256] = {0};
    int result = 1;
    wasm_module_t module = wasm_runtime_load(bytes, (uint32_t)size, error, sizeof(error));
    wasm_module_inst_t instance = module ? wasm_runtime_instantiate(module, 65536, 65536, error, sizeof(error)) : NULL;
    wasm_exec_env_t env = instance ? wasm_runtime_create_exec_env(instance, 65536) : NULL;
    if (!env) { fprintf(stderr, "runtime: %s\n", error); goto cleanup; }
    uint32_t args[1] = {0};
    if (!call(instance, env, "aster_app_init", 0, args) || args[0] != ASTER_OK) goto cleanup;
    if (!call(instance, env, "aster_app_start", 0, args)) goto cleanup;
    void *native = NULL;
    uint32_t event_offset = (uint32_t)wasm_runtime_module_malloc(instance, sizeof(aster_event_t), &native);
    if (!event_offset) goto cleanup;
    for (unsigned frame = 0; frame < 600 && !exited; ++frame) {
        aster_event_t *event = wasm_runtime_addr_app_to_native(instance, event_offset);
        memset(event, 0, sizeof(*event));
        event->size = sizeof(*event);
        event->version = 1;
        event->type = ASTER_EVENT_FRAME;
        event->timestamp_ms = now_ms(env);
        event->u.frame.frame = frame;
        args[0] = event_offset;
        if (!call(instance, env, "aster_app_event", 1, args)) goto cleanup;
        long period = 1000000000L / fps;
        struct timespec delay = {.tv_sec = period / 1000000000L, .tv_nsec = period % 1000000000L};
        nanosleep(&delay, NULL);
    }
    wasm_runtime_module_free(instance, event_offset);
    if (!call(instance, env, "aster_app_stop", 0, args)) goto cleanup;
    if (!call(instance, env, "aster_app_destroy", 0, args)) goto cleanup;
    result = exited ? exit_code : 124;
cleanup:
    if (env) wasm_runtime_destroy_exec_env(env);
    if (instance) wasm_runtime_deinstantiate(instance);
    if (module) wasm_runtime_unload(module);
    wasm_runtime_destroy();
    free(bytes);
    return result;
}
