/* SPDX-License-Identifier: MIT
 * Audio ABI adapted from systems/zephyr/runtime/aster_native.c. Platform calls
 * reuse the original player; owner, memory and sandbox helpers use the IDF VM.
 */
#include <zephyr/kernel.h>
#include <aster/aster.h>
#include <wasm_export.h>
#include "aster_platform.h"
#include "aster_storage.h"
#include "aster_perms.h"
#include "files.h"
#include <string.h>
#include <errno.h>
extern bool aster_sysinfo_has_feature(uint32_t bit);
typedef struct {uint32_t audio_stream_handle;} aster_app_ctx_t;
static aster_app_ctx_t context;
static aster_app_ctx_t *ctx_of(wasm_exec_env_t env) {(void)env;return &context;}
static bool aster_ctx_perm(aster_app_ctx_t *ctx,uint32_t p) {(void)ctx;return aster_idf_has_permission(p);}
static uint8_t *buf(wasm_exec_env_t env,uint32_t off,uint32_t n)
{
 wasm_module_inst_t inst=wasm_runtime_get_module_inst(env);uint64_t end;
 if(!off || !n || !wasm_runtime_get_app_addr_range(inst,off,NULL,&end) || (uint64_t)off+n>end)return NULL;
 return wasm_runtime_addr_app_to_native(inst,off);
}
static int app_str(wasm_exec_env_t env,uint32_t off,uint32_t len,char *out,size_t cap)
{
 if(!len || len>=cap)return -1;
 const char *p=(const char *)buf(env,off,len);if(!p || memchr(p,0,len))return -1;
 memcpy(out,p,len);out[len]=0;return 0;
}
static int sandbox_map(aster_app_ctx_t *ctx,const char *path,char *out,size_t size)
{
 (void)ctx;if(strchr(path,':') || strchr(path,'\\'))return ASTER_EPERM;
 uint32_t permissions=ASTER_PERM_FS|(aster_idf_has_permission(ASTER_PERM_SD)?ASTER_PERM_SD:0);
 return aster_storage_sandbox(out,size,aster_idf_app_identity(),path,permissions);
}
/* ============ audio (ABI 1.6 PCM, 1.7 volume/session, 1.8 MP3, 1.9 file) ============ */

static int32_t audio_out_gate(void)
{
	return aster_sysinfo_has_feature(ASTER_FEATURE_AUDIO_OUTPUT) ? ASTER_OK : ASTER_ENODEV;
}

static int32_t audio_in_gate(void)
{
	return aster_sysinfo_has_feature(ASTER_FEATURE_AUDIO_INPUT) ? ASTER_OK : ASTER_ENODEV;
}

#if IS_ENABLED(CONFIG_ASTER_AUDIO)
static int32_t map_audio_err(int err)
{
	if (err == 0) {
		return ASTER_OK;
	}
	if (err == -EINVAL) {
		return ASTER_EINVAL;
	}
	if (err == -ENODEV) {
		return ASTER_ENODEV;
	}
	if (err == -EBUSY) {
		return ASTER_EBUSY;
	}
	if (err == -EAGAIN) {
		return ASTER_ESTATE;
	}
	if (err == -ENOMEM) {
		return ASTER_ENOMEM;
	}
	if (err == -ENOENT) {
		return ASTER_ENOENT;
	}
	if (err == -ETIMEDOUT) {
		return ASTER_ETIMEDOUT;
	}
	return ASTER_EIO;
}
#endif

/* 2.0 removed n_audio_play with its import: playback goes through the
 * background session (start_file). aster_audio_record stays -- capture has no
 * streaming replacement.
 *
 * 3.2 restores n_audio_get_info, which 2.0 dropped on the grounds that the
 * format was fixed by constants. It is fixed, but per *board*, and an app is
 * compiled once and installed anywhere: the ASTER_AUDIO_* macros it used to
 * read were a number that is wrong on every board but one. Asking is the
 * only answer that survives a codec change. */
static int32_t audio_frame_bytes(uint32_t *frame)
{
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	uint32_t rate, ch, bits;

	if (aster_plat_audio_info(&rate, &ch, &bits) == 0) {
		*frame = ch * (bits / 8u);
		return ASTER_OK;
	}
#endif
	*frame = 0;
	return ASTER_ENODEV; /* capability bit without a driver behind it */
}

static int32_t n_audio_get_info(wasm_exec_env_t env, uint32_t off, uint32_t info_size)
{
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
	/* Same forward-compat shape as get_state below: size + version are the
	 * contract, so they are the floor, and a caller built against an older
	 * (shorter) struct gets the prefix it knows. */
	if (info_size < 8) {
		return ASTER_EINVAL;
	}
	size_t n = info_size < sizeof(aster_audio_info_t) ? info_size : sizeof(aster_audio_info_t);
	uint8_t *dst = buf(env, off, n);
	if (!dst) {
		return ASTER_EINVAL;
	}
	aster_audio_info_t info = {
		.size = sizeof(aster_audio_info_t),
		.version = 1,
	};
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	uint32_t rate, ch, bits;

	if (aster_plat_audio_info(&rate, &ch, &bits) != 0) {
		return ASTER_ENODEV;
	}
	info.sample_rate = rate;
	info.channels = ch;
	info.bits = bits;
	info.flags = 0;
#else
	return ASTER_ENODEV;
#endif
	memcpy(dst, &info, n);
	return ASTER_OK;
}

#include "../../../zephyr/runtime/aster_audio_stream_bridge.inc"

static int32_t n_audio_record(wasm_exec_env_t env, uint32_t off, uint32_t size)
{
	int32_t e = audio_in_gate();
	uint32_t frame;

	if (e != ASTER_OK) {
		return e;
	}
	e = audio_frame_bytes(&frame);
	if (e != ASTER_OK) {
		return e;
	}
	if (size == 0 || (size % frame) != 0) {
		return ASTER_EINVAL;
	}
	uint8_t *dst = buf(env, off, size);
	if (!dst) {
		return ASTER_EINVAL;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_record(dst, size));
#else
	ARG_UNUSED(dst);
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_set_volume(wasm_exec_env_t env, int32_t percent)
{
	ARG_UNUSED(env);
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_set_volume(percent));
#else
	ARG_UNUSED(percent);
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_get_volume(wasm_exec_env_t env)
{
	ARG_UNUSED(env);
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return aster_plat_audio_get_volume();
#else
	return ASTER_ENODEV;
#endif
}

/* 2.0: n_audio_start removed — the 2 MiB in-memory copy route is superseded
 * by start_file streaming (`hw audio` plays via aster_plat_audio_start_path). */

static int32_t n_audio_start_file(wasm_exec_env_t env, uint32_t off, uint32_t len)
{
	int32_t e = audio_out_gate();
	aster_app_ctx_t *ctx;
	char apath[160];
	char real[256];

	if (e != ASTER_OK) {
		return e;
	}
	ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EINVAL;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	if (app_str(env, off, len, apath, sizeof(apath)) < 0) {
		return ASTER_EINVAL;
	}
	if (sandbox_map(ctx, apath, real, sizeof(real)) != 0) {
		return ASTER_EPERM;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_start_path(real));
#else
	ARG_UNUSED(real);
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_effect_load(wasm_exec_env_t env, uint32_t off, uint32_t len)
{
	int32_t e = audio_out_gate();
	aster_app_ctx_t *ctx;
	char apath[160];
	char real[256];

	if (e != ASTER_OK) {
		return e;
	}
	ctx = ctx_of(env);
	if (!ctx || !aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	if (app_str(env, off, len, apath, sizeof(apath)) < 0 ||
	    sandbox_map(ctx, apath, real, sizeof(real)) != 0) {
		return ASTER_EINVAL;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	int handle = aster_plat_audio_effect_load(real);
	return handle > 0 ? handle : map_audio_err(handle);
#else
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_effect_load_memory(wasm_exec_env_t env,uint32_t off,uint32_t len)
{
 int32_t e=audio_out_gate();if(e!=ASTER_OK)return e;
 if(!ctx_of(env)||len<12||len>69632)return ASTER_EINVAL;
 const void *data=buf(env,off,len);if(!data)return ASTER_EINVAL;
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
 int h=aster_plat_audio_effect_load_memory(data,len);return h>0?h:map_audio_err(h);
#else
 return ASTER_ENODEV;
#endif
}

static int32_t n_audio_effect_play(wasm_exec_env_t env, uint32_t handle)
{
	ARG_UNUSED(env);
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_effect_play(handle));
#else
	return ASTER_ENODEV;
#endif
}

/* Copy versioned guest descriptors before entering the audio service. */
static int32_t n_audio_effect_play_ex(wasm_exec_env_t env, uint32_t handle,
                                     uint32_t off, uint32_t size)
{
 int32_t e=audio_out_gate();
 if(e!=ASTER_OK) return e;
 if(size!=sizeof(aster_audio_effect_play_desc_t)) return ASTER_EINVAL;
 const void *src=buf(env,off,size);
 if(!src) return ASTER_EINVAL;
 aster_audio_effect_play_desc_t d;memcpy(&d,src,sizeof(d));
 if(d.size!=size || d.version!=1) return ASTER_EINVAL;
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
 return map_audio_err(aster_plat_audio_effect_play_ex(handle,&d));
#else
 ARG_UNUSED(handle);return ASTER_ENODEV;
#endif
}
static int32_t n_audio_effect_set_params(wasm_exec_env_t env, uint32_t handle,
                                        uint32_t off, uint32_t size)
{
 int32_t e=audio_out_gate();
 if(e!=ASTER_OK) return e;
 if(size!=sizeof(aster_audio_effect_params_t)) return ASTER_EINVAL;
 const void *src=buf(env,off,size);
 if(!src) return ASTER_EINVAL;
 aster_audio_effect_params_t p;memcpy(&p,src,sizeof(p));
 if(p.size!=size || p.version!=1) return ASTER_EINVAL;
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
 return map_audio_err(aster_plat_audio_effect_set_params(handle,&p));
#else
 ARG_UNUSED(handle);return ASTER_ENODEV;
#endif
}
static int32_t n_audio_effect_stop(wasm_exec_env_t env,uint32_t handle,uint32_t ms)
{
 ARG_UNUSED(env);
 int32_t e=audio_out_gate();
 if(e!=ASTER_OK) return e;
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
 return map_audio_err(aster_plat_audio_effect_stop(handle,ms));
#else
 ARG_UNUSED(handle);ARG_UNUSED(ms);return ASTER_ENODEV;
#endif
}

static int32_t n_audio_effect_unload(wasm_exec_env_t env, uint32_t handle)
{
	ARG_UNUSED(env);
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_effect_unload(handle));
#else
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_pause(wasm_exec_env_t env)
{
	ARG_UNUSED(env);
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_pause());
#else
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_resume(wasm_exec_env_t env)
{
	ARG_UNUSED(env);
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_resume());
#else
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_stop(wasm_exec_env_t env)
{
	ARG_UNUSED(env);
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_stop());
#else
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_get_state(wasm_exec_env_t env, uint32_t off, uint32_t st_size)
{
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
	if (st_size < 8) {
		return ASTER_EINVAL;
	}
	size_t n = st_size < sizeof(aster_audio_state_t) ? st_size : sizeof(aster_audio_state_t);
	uint8_t *dst = buf(env, off, n);
	if (!dst) {
		return ASTER_EINVAL;
	}
	aster_audio_state_t st = {
		.size = sizeof(aster_audio_state_t),
		.version = 2,
	};
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	(void)aster_plat_audio_get_state(&st.state, &st.bytes_total, &st.bytes_done, &st.volume);
	(void)aster_plat_audio_get_clock(&st.msec_done, &st.msec_total);
#else
	return ASTER_ENODEV;
#endif
	memcpy(dst, &st, n);
	return ASTER_OK;
}

static int32_t n_audio_seek(wasm_exec_env_t env, uint32_t msec)
{
	ARG_UNUSED(env);
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_seek(msec));
#else
	ARG_UNUSED(msec);
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_get_lyrics(wasm_exec_env_t env, uint32_t off, uint32_t size)
{
	int32_t e = audio_out_gate();
	char *dst;

	if (e != ASTER_OK) {
		return e;
	}
	if (size == 0) {
		return ASTER_EINVAL;
	}
	dst = (char *)buf(env, off, size);
	if (!dst) {
		return ASTER_EINVAL;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	{
		int n = aster_plat_audio_get_lyrics(dst, size);

		if (n < 0) {
			return map_audio_err(n);
		}
		return (int32_t)n;
	}
#else
	ARG_UNUSED(dst);
	return ASTER_ENODEV;
#endif
}


void aster_idf_audio_cleanup(void)
{
 aster_native_release_audio_stream(&context);
 aster_plat_audio_stop();aster_plat_audio_effect_reset();
}
static NativeSymbol symbols[]={
	{ "aster_audio_get_info", (void *)n_audio_get_info, "(ii)i", NULL },
	{ "aster_audio_get_lyrics", (void *)n_audio_get_lyrics, "(ii)i", NULL },
	{ "aster_audio_get_state", (void *)n_audio_get_state, "(ii)i", NULL },
	{ "aster_audio_get_volume", (void *)n_audio_get_volume, "()i", NULL },
	{ "aster_audio_effect_load", (void *)n_audio_effect_load, "(ii)i", NULL },
	{ "aster_audio_effect_load_memory", (void *)n_audio_effect_load_memory, "(ii)i", NULL },
	{ "aster_audio_effect_play", (void *)n_audio_effect_play, "(i)i", NULL },
	{ "aster_audio_effect_play_ex", (void *)n_audio_effect_play_ex, "(iii)i", NULL },
	{ "aster_audio_effect_set_params", (void *)n_audio_effect_set_params, "(iii)i", NULL },
	{ "aster_audio_effect_stop", (void *)n_audio_effect_stop, "(ii)i", NULL },
	{ "aster_audio_effect_unload", (void *)n_audio_effect_unload, "(i)i", NULL },
	{ "aster_audio_pause", (void *)n_audio_pause, "()i", NULL },
	{ "aster_audio_record", (void *)n_audio_record, "(ii)i", NULL },
	{ "aster_audio_resume", (void *)n_audio_resume, "()i", NULL },
	{ "aster_audio_seek", (void *)n_audio_seek, "(i)i", NULL },
	{ "aster_audio_set_volume", (void *)n_audio_set_volume, "(i)i", NULL },
	{ "aster_audio_start_file", (void *)n_audio_start_file, "(ii)i", NULL },
	{ "aster_audio_stream_open", (void *)n_audio_stream_open, "(i)i", NULL },
	{ "aster_audio_stream_write", (void *)n_audio_stream_write, "(iii)i", NULL },
	{ "aster_audio_stream_finish", (void *)n_audio_stream_finish, "(i)i", NULL },
	{ "aster_audio_stream_status", (void *)n_audio_stream_status, "(iii)i", NULL },
	{ "aster_audio_stream_close", (void *)n_audio_stream_close, "(i)i", NULL },
	{ "aster_audio_stop", (void *)n_audio_stop, "()i", NULL },
};
NativeSymbol *aster_idf_audio_symbols(uint32_t *n) {*n=sizeof(symbols)/sizeof(symbols[0]);return symbols;}
