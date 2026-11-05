/* SPDX-License-Identifier: MIT */
#ifndef ASTER_IDF_AUX_STACK_H
#define ASTER_IDF_AUX_STACK_H
#include "wasm_export.h"

/* WAMR internal helpers in the bundled version. Thread management is enabled
 * for asynchronous termination, but Aster exposes neither guest thread APIs
 * nor additional execution contexts. Its default cluster partitions the C
 * auxiliary stack into five pieces anyway. Restore the declared full region
 * for our sole context; keep AOT's upper/lower bound checks enabled.
 * Revisit this policy before introducing guest threads or spawned exec_envs. */
#if WASM_ENABLE_THREAD_MGR != 0
extern bool wasm_exec_env_get_aux_stack(wasm_exec_env_t env,
                                      uint64_t *start, uint32_t *size);
extern bool wasm_exec_env_set_aux_stack(wasm_exec_env_t env,
                                      uint64_t start, uint32_t size);
#endif
static inline bool aster_idf_bind_main_aux_stack(wasm_exec_env_t env)
{
#if WASM_ENABLE_THREAD_MGR != 0
    uint64_t start;
    uint32_t size;
    /* Small hand-written modules can legitimately have no compiler C stack. */
    if (wasm_exec_env_get_aux_stack(env, &start, &size))
        return wasm_exec_env_set_aux_stack(env, start, size);
#else
    (void)env;
#endif
    return true;
}
#endif
