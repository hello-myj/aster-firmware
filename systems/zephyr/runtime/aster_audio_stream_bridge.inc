/* SPDX-License-Identifier: MIT. Worker receives copied compressed bytes only. */
static int32_t stream_err(int rc)
{
    if (rc >= 0)
        return rc;
    switch (-rc) {
    case EAGAIN:
        return ASTER_EAGAIN;
    case EBADF:
        return ASTER_EBADF;
    case EPIPE:
        return ASTER_EPIPE;
    case EPROTO:
        return ASTER_EPROTO;
    case ECANCELED:
        return ASTER_ECANCELLED;
    case ENOSPC:
        return ASTER_ENOSPC;
    case ENOTSUP:
        return ASTER_ENOTSUP;
    case EMSGSIZE:
        return ASTER_EMSGSIZE;
    case EINVAL:
        return ASTER_EINVAL;
    case ENOMEM:
        return ASTER_ENOMEM;
    case EBUSY:
        return ASTER_EBUSY;
    case ENODEV:
        return ASTER_ENODEV;
    case ETIMEDOUT:
        return ASTER_ETIMEDOUT;
    default:
        return ASTER_EIO;
    }
}

static int32_t n_audio_stream_open(wasm_exec_env_t env, uint32_t codec)
{
    int rc = audio_out_gate();
    if (rc)
        return rc;
    aster_app_ctx_t *ctx = ctx_of(env);
    if (!ctx)
        return ASTER_EINVAL;
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
    if (ctx->audio_stream_handle)
        return ASTER_EBUSY;
    rc = aster_plat_audio_stream_open(codec);
    if (rc > 0)
        ctx->audio_stream_handle = rc;
    return stream_err(rc);
#else
    (void)codec;
    return ASTER_ENODEV;
#endif
}

static int32_t stream_owner(wasm_exec_env_t env, uint32_t handle)
{
    int rc = audio_out_gate();
    if (rc)
        return rc;
    aster_app_ctx_t *ctx = ctx_of(env);
    return ctx && handle && ctx->audio_stream_handle == handle ? 0 : ASTER_EBADF;
}

static uint8_t *stream_buffer(wasm_exec_env_t env, uint32_t off, uint32_t size)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    uint64_t end = 0;
    if (!inst || !off || !size || !wasm_runtime_get_app_addr_range(inst, off, NULL, &end) ||
        (uint64_t)off + size > end)
        return NULL;
    return wasm_runtime_addr_app_to_native(inst, off);
}

static int32_t n_audio_stream_write(wasm_exec_env_t env, uint32_t h, uint32_t off, uint32_t n)
{
    int rc = stream_owner(env, h);
    if (rc)
        return rc;
    if (!n || n > ASTER_AUDIO_STREAM_WRITE_MAX)
        return ASTER_EINVAL;
    uint8_t *p = stream_buffer(env, off, n);
    if (!p)
        return ASTER_EINVAL;
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
    return stream_err(aster_plat_audio_stream_write(h, p, n));
#else
    return ASTER_ENODEV;
#endif
}

static int32_t n_audio_stream_finish(wasm_exec_env_t env, uint32_t h)
{
    int rc = stream_owner(env, h);
    if (rc)
        return rc;
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
    return stream_err(aster_plat_audio_stream_finish(h));
#else
    return ASTER_ENODEV;
#endif
}

static int32_t n_audio_stream_status(wasm_exec_env_t env, uint32_t h, uint32_t off, uint32_t size)
{
    int rc = stream_owner(env, h);
    if (rc)
        return rc;
    uint8_t *p = size >= sizeof(aster_audio_stream_status_t)
                     ? stream_buffer(env, off, sizeof(aster_audio_stream_status_t))
                     : NULL;
    if (!p)
        return ASTER_EINVAL;
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
    aster_audio_stream_status_t s;
    rc = aster_plat_audio_stream_status(h, &s);
    if (rc)
        return stream_err(rc);
    s.error = stream_err(s.error);
    memcpy(p, &s, sizeof(s));
    return 0;
#else
    return ASTER_ENODEV;
#endif
}

static int32_t n_audio_stream_close(wasm_exec_env_t env, uint32_t h)
{
    int rc = stream_owner(env, h);
    if (rc)
        return rc;
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
    rc = aster_plat_audio_stream_close(h);
    if (!rc)
        ctx_of(env)->audio_stream_handle = 0;
    return stream_err(rc);
#else
    return ASTER_ENODEV;
#endif
}

static int32_t n_audio_capture_open(wasm_exec_env_t env, uint32_t off, uint32_t size)
{
    int rc = audio_in_gate();
    if (rc)
        return rc;
    if (!aster_device_has(ASTER_DEVICE_METHOD_audio_capture_open))
        return ASTER_ENOTSUP;
    aster_app_ctx_t *ctx = ctx_of(env);
    if (!ctx || size < sizeof(aster_audio_info_t))
        return ASTER_EINVAL;
    uint8_t *out = stream_buffer(env, off, sizeof(aster_audio_info_t));
    if (!out)
        return ASTER_EINVAL;
    if (ctx->audio_capture_handle)
        return ASTER_EBUSY;
    aster_audio_info_t info;
    rc = aster_device_audio_capture_open(&info);
    if (rc > 0) {
        ctx->audio_capture_handle = (uint32_t)rc;
        memcpy(out, &info, sizeof(info));
    }
    return stream_err(rc);
}

static bool owns_capture(wasm_exec_env_t env, uint32_t handle)
{
    aster_app_ctx_t *ctx = ctx_of(env);
    return ctx && handle && ctx->audio_capture_handle == handle;
}

static int32_t n_audio_capture_read(wasm_exec_env_t env, uint32_t handle, uint32_t off, uint32_t n)
{
    if (!owns_capture(env, handle))
        return ASTER_EBADF;
    uint8_t *dst = stream_buffer(env, off, n);
    if (!dst || n > INT32_MAX)
        return ASTER_EINVAL;
    return stream_err(aster_device_audio_capture_read(handle, dst, n));
}

static int32_t n_audio_capture_finish(wasm_exec_env_t env, uint32_t handle)
{
    return owns_capture(env, handle) ? stream_err(aster_device_audio_capture_finish(handle)) : ASTER_EBADF;
}

static int32_t n_audio_capture_close(wasm_exec_env_t env, uint32_t handle)
{
    if (!owns_capture(env, handle))
        return ASTER_EBADF;
    int rc = aster_device_audio_capture_close(handle);
    if (!rc)
        ctx_of(env)->audio_capture_handle = 0;
    return stream_err(rc);
}

void aster_native_release_audio_stream(aster_app_ctx_t *ctx)
{
    if (ctx && ctx->audio_capture_handle) {
        aster_device_audio_capture_close(ctx->audio_capture_handle);
        ctx->audio_capture_handle = 0;
    }
    if (!ctx || !ctx->audio_stream_handle)
        return;
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
    while (aster_plat_audio_stream_close(ctx->audio_stream_handle) == -EAGAIN)
        k_sleep(K_MSEC(1));
#endif
    ctx->audio_stream_handle = 0;
}
