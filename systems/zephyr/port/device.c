/* SPDX-License-Identifier: MIT */
#include "aster_device.h"
#include <zephyr/sys/util.h>
const aster_device_ops_t aster_device_backend = {
 .size=sizeof(aster_device_ops_t), .version=ASTER_DEVICE_API_VERSION,
 .name="zephyr",
 .display_count=aster_plat_display_count,
 .display_set_enabled=aster_plat_display_set_enabled,
 .display_get_info=aster_plat_display_get_info,
 .display_write_rgb565=aster_plat_display_write_rgb565,
 .display_write_app_regions=aster_plat_display_write_app_regions,
 .display_generation=aster_plat_display_generation,
 .display_window_test=aster_plat_display_window_test,
 .display_qspi_tune=aster_plat_display_qspi_tune,
 .display_write_at=aster_plat_display_write_at,
 .display_write_at_ex=aster_plat_display_write_at_ex,
 .display_write_persistent_ui=aster_plat_display_write_persistent_ui,
 .display_flip_rgb565=aster_plat_display_flip_rgb565,
 .display_canvas_ready=aster_plat_display_canvas_ready,
 .display_canvas_back=aster_plat_display_canvas_back,
 .display_canvas_present=aster_plat_display_canvas_present,
 .display_flip_acquire=aster_plat_display_flip_acquire,
 .display_flip_release=aster_plat_display_flip_release,
 .display_app_space=aster_plat_display_app_space,
 .display_app_scaled=aster_plat_display_app_scaled,
 .display_app_touch=aster_plat_display_app_touch,
 .display_write_app_at=aster_plat_display_write_app_at,
 .display_app_present=aster_plat_display_app_present,
 .display_scale_fit_present=aster_plat_display_scale_fit_present,
 .display_scale_fit_disarm=aster_plat_display_scale_fit_disarm,
 .display_glass_frame=aster_plat_display_glass_frame,
 .display_glass_rearm=aster_plat_display_glass_rearm,
 .display_glass_drop=aster_plat_display_glass_drop,
 .display_flush_drain=aster_plat_display_flush_drain,
 .display_flush_drain_checked=aster_plat_display_flush_drain_checked,
 .display_rotation_set=aster_plat_display_rotation_set,
 .display_rotation_get=aster_plat_display_rotation_get,
 .display_orientation_set=aster_plat_display_orientation_set,
 .display_orientation_get=aster_plat_display_orientation_get,
 .display_touch_map=aster_plat_display_touch_map,
 .display_touch_unmap=aster_plat_display_touch_unmap,
 .backlight_set=aster_plat_backlight_set,
 .display_clear=aster_plat_display_clear,
 .input_poll=aster_plat_input_poll,
 .input_touch_available=aster_plat_input_touch_available,
 .input_state=aster_plat_input_state,
 .input_touch_inject=aster_plat_input_touch_inject,
 .input_touch_inject_slot=aster_plat_input_touch_inject_slot,
 .input_touch_debug=aster_plat_input_touch_debug,
#if defined(CONFIG_ASTER_AXS15231B_RAW_DIAG) && CONFIG_ASTER_AXS15231B_RAW_DIAG
 .axs_touch_diag_replay_start=aster_plat_axs_touch_diag_replay_start,
#endif
 .input_touch_report_frame=aster_plat_input_touch_report_frame,
 .input_touch_report_error=aster_plat_input_touch_report_error,
 .input_touch_set_ready=aster_plat_input_touch_set_ready,
 .input_touch_max_points=aster_plat_input_touch_max_points,
 .input_keys_poll=aster_plat_input_keys_poll,
 .input_keys_available=aster_plat_input_keys_available,
 .input_keys_state=aster_plat_input_keys_state,
 .input_keys_inject=aster_plat_input_keys_inject,
 .imu_read=aster_plat_imu_read,
#if defined(CONFIG_ASTER_CAMERA) && CONFIG_ASTER_CAMERA
 .cam_info=aster_plat_cam_info,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_info=aster_plat_audio_info,
#endif
#if defined(CONFIG_ASTER_CAMERA) && CONFIG_ASTER_CAMERA
 .cam_capture_rgb565=aster_plat_cam_capture_rgb565,
#endif
#if defined(CONFIG_ASTER_CAMERA) && CONFIG_ASTER_CAMERA
 .cam_grab_rgb565=aster_plat_cam_grab_rgb565,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_play=aster_plat_audio_play,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_record=aster_plat_audio_record,
    .audio_capture_open = aster_plat_audio_capture_open,
    .audio_capture_read = aster_plat_audio_capture_read,
    .audio_capture_finish = aster_plat_audio_capture_finish,
    .audio_capture_close = aster_plat_audio_capture_close,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_beep=aster_plat_audio_beep,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_set_volume=aster_plat_audio_set_volume,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_get_volume=aster_plat_audio_get_volume,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_ui_play=aster_plat_audio_ui_play,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_ui_stop=aster_plat_audio_ui_stop,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_ui_release=aster_plat_audio_ui_release,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_effect_load=aster_plat_audio_effect_load,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_effect_load_memory=aster_plat_audio_effect_load_memory,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_effect_play=aster_plat_audio_effect_play,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_effect_play_ex=aster_plat_audio_effect_play_ex,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_effect_set_params=aster_plat_audio_effect_set_params,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_effect_stop=aster_plat_audio_effect_stop,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_effect_suspend=aster_plat_audio_effect_suspend,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_effect_debug=aster_plat_audio_effect_debug,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_effect_unload=aster_plat_audio_effect_unload,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_effect_reset=aster_plat_audio_effect_reset,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_start_path=aster_plat_audio_start_path,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_play_path=aster_plat_audio_play_path,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_pause=aster_plat_audio_pause,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_resume=aster_plat_audio_resume,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_stop=aster_plat_audio_stop,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_get_state=aster_plat_audio_get_state,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_get_clock=aster_plat_audio_get_clock,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_seek=aster_plat_audio_seek,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_get_lyrics=aster_plat_audio_get_lyrics,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_stream_open=aster_plat_audio_stream_open,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_stream_write=aster_plat_audio_stream_write,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_stream_finish=aster_plat_audio_stream_finish,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_stream_status=aster_plat_audio_stream_status,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_stream_close=aster_plat_audio_stream_close,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_is_mp3=aster_plat_audio_is_mp3,
#endif
#if defined(CONFIG_ASTER_AUDIO) && CONFIG_ASTER_AUDIO
 .audio_probe=aster_plat_audio_probe,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_scan=aster_plat_wifi_scan,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_connect=aster_plat_wifi_connect,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_disconnect=aster_plat_wifi_disconnect,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_status=aster_plat_wifi_status,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_sec_txt=aster_plat_wifi_sec_txt,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_security_class=aster_plat_wifi_security_class,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_state_class=aster_plat_wifi_state_class,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_hotspot_start=aster_plat_wifi_hotspot_start,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_hotspot_stop=aster_plat_wifi_hotspot_stop,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_hotspot_status=aster_plat_wifi_hotspot_status,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_scan_start=aster_plat_wifi_scan_start,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_scan_results=aster_plat_wifi_scan_results,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_connect_start=aster_plat_wifi_connect_start,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_disconnect_start=aster_plat_wifi_disconnect_start,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_reconnect_start=aster_plat_wifi_reconnect_start,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_forget_start=aster_plat_wifi_forget_start,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_operation=aster_plat_wifi_operation,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_storage_ready=aster_plat_wifi_storage_ready,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_forget=aster_plat_wifi_forget,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_last_ssid=aster_plat_wifi_last_ssid,
#endif
#if defined(CONFIG_ASTER_WIFI) && CONFIG_ASTER_WIFI
 .wifi_reconnect=aster_plat_wifi_reconnect,
#endif
};
