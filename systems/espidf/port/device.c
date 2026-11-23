/* SPDX-License-Identifier: MIT */
#include "aster_device.h"
#include "aster_idf_runtime.h"
#include "graphics.h"
#include <zephyr/sys/util.h>

static bool touch_available(void)
{
    return aster_idf_feature(ASTER_FEATURE_TOUCH) != 0;
}

const aster_device_ops_t aster_device_backend = {
    .size = sizeof(aster_device_ops_t),
    .version = ASTER_DEVICE_API_VERSION,
    .name = "espidf",
    .imu_read = aster_idf_imu_read,
    .display_count = aster_plat_display_count,
    .display_get_info = aster_plat_display_get_info,
    .display_write_at = aster_plat_display_write_at,
    .display_write_at_ex = aster_plat_display_write_at_ex,
    .display_glass_frame = aster_plat_display_glass_frame,
    .display_flush_drain = aster_plat_display_flush_drain,
    .display_orientation_set = aster_plat_display_orientation_set,
    .display_orientation_get = aster_plat_display_orientation_get,
    .backlight_set = aster_plat_backlight_set,
    .display_clear = aster_plat_display_clear,
    .input_poll = aster_plat_input_poll,
    .input_touch_available = touch_available,
    .input_touch_max_points = aster_idf_touch_capacity,
    .input_keys_poll = aster_plat_input_keys_poll,
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_info = aster_plat_audio_info,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_play = aster_plat_audio_play,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_record = aster_plat_audio_record,
    .audio_capture_open = aster_plat_audio_capture_open,
    .audio_capture_read = aster_plat_audio_capture_read,
    .audio_capture_finish = aster_plat_audio_capture_finish,
    .audio_capture_close = aster_plat_audio_capture_close,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_beep = aster_plat_audio_beep,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_set_volume = aster_plat_audio_set_volume,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_get_volume = aster_plat_audio_get_volume,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_ui_play = aster_plat_audio_ui_play,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_ui_stop = aster_plat_audio_ui_stop,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_ui_release = aster_plat_audio_ui_release,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_effect_load = aster_plat_audio_effect_load,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_effect_load_memory = aster_plat_audio_effect_load_memory,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_effect_play = aster_plat_audio_effect_play,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_effect_play_ex = aster_plat_audio_effect_play_ex,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_effect_set_params = aster_plat_audio_effect_set_params,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_effect_stop = aster_plat_audio_effect_stop,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_effect_suspend = aster_plat_audio_effect_suspend,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_effect_debug = aster_plat_audio_effect_debug,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_effect_unload = aster_plat_audio_effect_unload,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_effect_reset = aster_plat_audio_effect_reset,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_start_path = aster_plat_audio_start_path,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_play_path = aster_plat_audio_play_path,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_pause = aster_plat_audio_pause,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_resume = aster_plat_audio_resume,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_stop = aster_plat_audio_stop,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_get_state = aster_plat_audio_get_state,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_get_clock = aster_plat_audio_get_clock,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_seek = aster_plat_audio_seek,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_get_lyrics = aster_plat_audio_get_lyrics,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_stream_open = aster_plat_audio_stream_open,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_stream_write = aster_plat_audio_stream_write,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_stream_finish = aster_plat_audio_stream_finish,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_stream_status = aster_plat_audio_stream_status,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_stream_close = aster_plat_audio_stream_close,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_is_mp3 = aster_plat_audio_is_mp3,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
    .audio_probe = aster_plat_audio_probe,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
    .wifi_status = aster_plat_wifi_status,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
    .wifi_sec_txt = aster_plat_wifi_sec_txt,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
    .wifi_security_class = aster_plat_wifi_security_class,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
    .wifi_state_class = aster_plat_wifi_state_class,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
    .wifi_hotspot_start = aster_plat_wifi_hotspot_start,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
    .wifi_hotspot_stop = aster_plat_wifi_hotspot_stop,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
    .wifi_hotspot_status = aster_plat_wifi_hotspot_status,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
    .wifi_scan_start = aster_plat_wifi_scan_start,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
    .wifi_scan_results = aster_plat_wifi_scan_results,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
    .wifi_connect_start = aster_plat_wifi_connect_start,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
    .wifi_disconnect_start = aster_plat_wifi_disconnect_start,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
    .wifi_reconnect_start = aster_plat_wifi_reconnect_start,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
    .wifi_forget_start = aster_plat_wifi_forget_start,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
    .wifi_operation = aster_plat_wifi_operation,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
    .wifi_storage_ready = aster_plat_wifi_storage_ready,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
    .wifi_last_ssid = aster_plat_wifi_last_ssid,
#endif
};
