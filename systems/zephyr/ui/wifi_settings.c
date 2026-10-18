/* SPDX-License-Identifier: MIT
 * Native Wi-Fi and personal-hotspot settings. Radio work stays off the UI thread.
 */
#include "wifi_settings.h"
#include "aster_platform.h"
#include "ui_font.h"
#include "font_sd.h"
#include "ui_lvgl.h"
#include "ui_wifi.h"
#include "aster_mdns.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

#if IS_ENABLED(CONFIG_ASTER_LAUNCHER_FLUX)
#define BG 0x181b18
#define PANEL 0x252d22
#define FG 0xf3f3e8
#define MUTED 0x879376
#define ACCENT 0xd8ff8c
#define GREEN 0x89eac6
#define RULE 0x394330
#define RED 0xff9898
#else
#define BG 0x11151c
#define PANEL 0x1c232d
#define FG 0xf1f4f8
#define MUTED 0x96a3b5
#define ACCENT 0x7db2ff
#define GREEN 0x79d4a0
#define RULE 0x2c3542
#define RED 0xff9898
#endif

enum page { WIFI_LIST, WIFI_JOIN, WIFI_DETAILS, WIFI_FORGET_CONFIRM, HOTSPOT, HOTSPOT_EDIT };
static enum page page;
static void (*owner_close)(void);
static lv_obj_t *root, *connection_name, *connection_info, *networks, *scan_label;
static lv_obj_t *ssid_field, *password_field, *keyboard, *input, *hint, *toast;
static lv_obj_t *hotspot_switch, *hotspot_name, *hotspot_info;
static aster_wifi_ap_t aps[ASTER_WIFI_AP_MAX];
static int ap_count, w, h;
static uint32_t pending;
static uint32_t toast_until;
static bool caps, numeric, symbols;
static bool require_password;
static aster_wifi_hotspot_status_t ap_state;
static void show(enum page next, const char *ssid);

/* clang-format off */
static const char *const letters[] = {
 "q","w","e","r","t","y","u","i","o","p","\n",
 "a","s","d","f","g","h","j","k","l","\n",
 "z","x","c","v","b","n","m","-","_","\n",
 "123","Aa","空格","删除","完成",""
};
static const char *const capitals[] = {
 "Q","W","E","R","T","Y","U","I","O","P","\n",
 "A","S","D","F","G","H","J","K","L","\n",
 "Z","X","C","V","B","N","M","-","_","\n",
 "123","Aa","空格","删除","完成",""
};
static const char *const digits[] = {
 "1","2","3","4","5","6","7","8","9","0","\n",
 "!","@","#","$","%","^","&","*","(",")","\n",
 "+","=","[","]","{","}","/","\\",".",",","\n",
 "abc","#+=","空格","删除","完成",""
};
static const char *const punctuation[] = {
 ":",";","'","\"","?","<",">","|","~","`","\n",
 "!","@","#","$","%","^","&","*","(",")","\n",
 "+","=","[","]","{","}","/","\\",".",",","\n",
 "abc","123","空格","删除","完成",""
};
/* clang-format on */

static void style(lv_obj_t *o, int x, int y, int width, int height, uint32_t color)
{
	lv_obj_set_pos(o, x, y);
	lv_obj_set_size(o, width, height);
	lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	lv_obj_set_style_pad_all(o, 0, 0);
	lv_obj_set_style_shadow_width(o, 0, 0);
	lv_obj_set_style_text_font(o, aster_ui_font(16), 0);
	lv_obj_set_style_text_color(o, lv_color_hex(FG), 0);
	lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}
static lv_obj_t *text(lv_obj_t *parent, const char *s, int x, int y, int width, uint32_t color)
{
	lv_obj_t *o = lv_label_create(parent);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_width(o, width);
	lv_obj_set_style_text_font(o, aster_ui_font(16), 0);
	lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
	lv_label_set_text(o, s);
	return o;
}
static void set_text(lv_obj_t *o, const char *s)
{
	if (o && strcmp(lv_label_get_text(o), s))
		lv_label_set_text(o, s);
}
static lv_obj_t *button(lv_obj_t *parent, const char *s, int x, int y, int width, int height,
			lv_event_cb_t cb, void *data)
{
	lv_obj_t *o = lv_button_create(parent);
	style(o, x, y, width, height, PANEL);
	lv_obj_set_style_radius(o, 7, 0);
	lv_obj_set_style_bg_color(o, lv_color_hex(RULE), LV_STATE_PRESSED);
	lv_obj_t *l = text(o, s, 0, 0, width, FG);
	lv_obj_center(l);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, data);
	return o;
}
static void notify(const char *s)
{
	if (hint) {
		set_text(hint, s);
		return;
	}
	if (!toast) {
		toast = lv_obj_create(root);
		lv_obj_add_flag(toast, LV_OBJ_FLAG_FLOATING);
		style(toast, 8, h - 64, w - 16, 56, RULE);
		lv_obj_set_style_radius(toast, 8, 0);
		text(toast, "", 8, 8, w - 32, FG);
	}
	set_text(lv_obj_get_child(toast, 0), s);
	lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(toast);
	toast_until = k_uptime_get_32() + 4000;
}
static void failed(int err)
{
	if (keyboard)
		lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
	switch (err) {
	case -EBUSY:
		notify("正在处理，请稍候");
		break;
	case -EINVAL:
		notify("名称最多32字节，密码8–63位");
		break;
	case -ENOENT:
		notify("尚未保存网络，请先连接");
		break;
	case -ETIMEDOUT:
		notify("连接超时，请检查网络与密码");
		break;
	default:
		notify("连接失败，请检查密码后重试");
		break;
	}
}
static void close_cb(lv_event_t *e)
{
	(void)e;
	if (owner_close)
		owner_close();
	else if (root)
		lv_obj_delete(root);
}
static void wifi_cb(lv_event_t *e)
{
	(void)e;
	show(WIFI_LIST, NULL);
}
static void hotspot_cb(lv_event_t *e)
{
	(void)e;
	show(HOTSPOT, NULL);
}
static void manual_cb(lv_event_t *e)
{
	(void)e;
	require_password = false;
	show(WIFI_JOIN, "");
}
static void details_cb(lv_event_t *e)
{
	(void)e;
	show(WIFI_DETAILS, NULL);
}
static void scan_cb(lv_event_t *e)
{
	(void)e;
	int err = aster_plat_wifi_scan_start();
	if (err)
		failed(err);
	else {
		pending = ASTER_PLAT_WIFI_OP_SCAN;
		set_text(scan_label, "扫描中");
	}
}
static void action_cb(lv_event_t *e)
{
	uint32_t op = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
	int err = op == ASTER_PLAT_WIFI_OP_FORGET      ? aster_plat_wifi_forget_start()
		  : op == ASTER_PLAT_WIFI_OP_RECONNECT ? aster_plat_wifi_reconnect_start()
						       : aster_plat_wifi_disconnect_start();
	if (err)
		failed(err);
	else {
		pending = op;
		show(WIFI_LIST, NULL);
	}
}
static void forget_cb(lv_event_t *e)
{
	(void)e;
	show(WIFI_FORGET_CONFIRM, NULL);
}
static void cancel_forget_cb(lv_event_t *e)
{
	(void)e;
	show(WIFI_DETAILS, NULL);
}
static void select_cb(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	uint32_t security = aster_plat_wifi_security_class(aps[i].security);
	if (security != ASTER_PLAT_WIFI_SECURITY_OPEN &&
	    security != ASTER_PLAT_WIFI_SECURITY_PERSONAL) {
		notify("暂不支持此网络的认证方式");
		return;
	}
	require_password = security != ASTER_PLAT_WIFI_SECURITY_OPEN;
	show(WIFI_JOIN, aps[i].ssid);
}
static void line(lv_obj_t *parent, int x, int y, int width)
{
	lv_obj_t *o = lv_obj_create(parent);
	style(o, x, y, width, 1, RULE);
	lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
}
static void signal(lv_obj_t *parent, int x, int y, int8_t rssi)
{
	int bars = rssi >= -55 ? 3 : rssi >= -72 ? 2 : 1;
	for (int i = 0; i < 3; i++) {
		lv_obj_t *o = lv_obj_create(parent);
		style(o, x + i * 6, y + 14 - (i + 1) * 4, 4, (i + 1) * 4, i < bars ? MUTED : RULE);
		lv_obj_set_style_radius(o, 1, 0);
		lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
	}
}
static void lock_icon(lv_obj_t *parent, int x, int y)
{
	lv_obj_t *o = lv_obj_create(parent);
	style(o, x + 2, y, 6, 8, BG);
	lv_obj_set_style_border_width(o, 1, 0);
	lv_obj_set_style_border_color(o, lv_color_hex(MUTED), 0);
	lv_obj_set_style_radius(o, 4, 0);
	lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
	o = lv_obj_create(parent);
	style(o, x, y + 5, 10, 8, MUTED);
	lv_obj_set_style_radius(o, 2, 0);
	lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
}
static void list_rows(void)
{
	if (!networks)
		return;
	lv_obj_clean(networks);
	aster_wifi_status_t st = {0};
	aster_plat_wifi_status(&st);
	for (int i = 0; i < ap_count; i++) {
		lv_obj_t *row =
		    button(networks, "", 0, i * 36, w - 8, 36, select_cb, (void *)(intptr_t)i);
		lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
		lv_obj_set_style_radius(row, 0, 0);
		lv_obj_t *name =
		    text(row, aps[i].ssid, 8, 8, w - 64,
			 st.state == WIFI_STATE_COMPLETED && !strcmp(st.ssid, aps[i].ssid) ? ACCENT
											   : FG);
		lv_obj_set_height(name, 21);
		lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
		if (aster_plat_wifi_security_class(aps[i].security) !=
		    ASTER_PLAT_WIFI_SECURITY_OPEN)
			lock_icon(row, w - 49, 12);
		signal(row, w - 32, 10, aps[i].rssi);
		line(row, 8, 35, w - 24);
	}
	if (!ap_count)
		text(networks, "未发现网络，点击扫描重试", 8, 14, w - 24, MUTED);
}
static void field_focus(lv_obj_t *o)
{
	input = o;
	lv_obj_set_style_border_width(ssid_field, input == ssid_field ? 1 : 0, 0);
	lv_obj_set_style_border_width(password_field, input == password_field ? 1 : 0, 0);
	lv_obj_add_state(input, LV_STATE_FOCUSED);
	lv_obj_clear_state(input == ssid_field ? password_field : ssid_field, LV_STATE_FOCUSED);
	lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}
static void field_cb(lv_event_t *e) { field_focus(lv_event_get_target_obj(e)); }
static void reveal_cb(lv_event_t *e)
{
	bool hidden = lv_textarea_get_password_mode(password_field);
	lv_textarea_set_password_mode(password_field, !hidden);
	set_text(lv_obj_get_child(lv_event_get_target_obj(e), 0), hidden ? "隐藏" : "显示");
}
static void key_cb(lv_event_t *e)
{
	(void)e;
	uint32_t i = lv_buttonmatrix_get_selected_button(keyboard);
	const char *key = lv_buttonmatrix_get_button_text(keyboard, i);
	if (!key || !input)
		return;
	if (!strcmp(key, "完成")) {
		lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	if (!strcmp(key, "删除")) {
		lv_textarea_delete_char(input);
		return;
	}
	if (!strcmp(key, "Aa")) {
		caps = !caps;
		numeric = symbols = false;
	} else if (!strcmp(key, "123") || !strcmp(key, "abc") || !strcmp(key, "#+=")) {
		numeric = strcmp(key, "abc") != 0;
		symbols = !strcmp(key, "#+=");
	} else {
		lv_textarea_add_text(input, !strcmp(key, "空格") ? " " : key);
		return;
	}
	lv_buttonmatrix_set_map(keyboard, symbols   ? punctuation
					  : numeric ? digits
					  : caps    ? capitals
						    : letters);
}
static void submit_cb(lv_event_t *e)
{
	(void)e;
	const char *ssid = lv_textarea_get_text(ssid_field),
		   *psk = lv_textarea_get_text(password_field);
	if (page == WIFI_JOIN && require_password && !psk[0]) {
		failed(-EINVAL);
		return;
	}
	int err = page == HOTSPOT_EDIT ? aster_plat_wifi_hotspot_start(ssid, psk)
				       : aster_plat_wifi_connect_start(ssid, psk);
	if (err) {
		failed(err);
		return;
	}
	pending =
	    page == HOTSPOT_EDIT ? ASTER_PLAT_WIFI_OP_HOTSPOT_START : ASTER_PLAT_WIFI_OP_CONNECT;
	show(page == HOTSPOT_EDIT ? HOTSPOT : WIFI_LIST, NULL);
}
static void hotspot_edit_cb(lv_event_t *e)
{
	(void)e;
	show(HOTSPOT_EDIT, ap_state.ssid);
}
static void hotspot_toggle_cb(lv_event_t *e)
{
	lv_obj_t *sw = lv_event_get_target_obj(e);
	bool enable = lv_obj_has_state(sw, LV_STATE_CHECKED);
	if (enable && !ap_state.configured) {
		show(HOTSPOT_EDIT, ap_state.ssid);
		return;
	}
	int err =
	    enable ? aster_plat_wifi_hotspot_start(NULL, NULL) : aster_plat_wifi_hotspot_stop();
	if (err) {
		failed(err);
		if (enable)
			lv_obj_clear_state(sw, LV_STATE_CHECKED);
		else
			lv_obj_add_state(sw, LV_STATE_CHECKED);
	} else
		pending =
		    enable ? ASTER_PLAT_WIFI_OP_HOTSPOT_START : ASTER_PLAT_WIFI_OP_HOTSPOT_STOP;
}
static lv_obj_t *field(int y, const char *value, int max)
{
	lv_obj_t *o = lv_textarea_create(root);
	style(o, 12, y, w - 24, 36, PANEL);
	lv_obj_set_style_radius(o, 8, 0);
	lv_obj_set_style_border_color(o, lv_color_hex(ACCENT), 0);
	lv_obj_set_style_pad_left(o, 8, 0);
	lv_obj_set_style_pad_top(o, 7, 0);
	lv_textarea_set_one_line(o, true);
	lv_textarea_set_max_length(o, max);
	lv_textarea_set_text(o, value);
	lv_obj_add_event_cb(o, field_cb, LV_EVENT_CLICKED, NULL);
	return o;
}
static void header(const char *title, lv_event_cb_t cb)
{
	lv_obj_t *l = text(root, title, 12, 8, w - 66, FG);
	lv_obj_set_style_text_font(l, aster_ui_font(20), 0);
	button(root, page == WIFI_LIST ? "关闭" : "返回", w - 54, 6, 46, 28, cb, NULL);
}
static void reset_view(void)
{
	connection_name = connection_info = networks = scan_label = NULL;
	ssid_field = password_field = keyboard = input = hint = toast = NULL;
	hotspot_switch = hotspot_name = hotspot_info = NULL;
	if (root)
		lv_obj_clean(root);
}
static void show(enum page next, const char *ssid)
{
	char selected[33] = {0};
	if (ssid)
		snprintf(selected, sizeof(selected), "%s", ssid);
	reset_view();
	/* Cache lifetime follows the screen owner, not this settings page. */
	page = next;
	aster_plat_wifi_hotspot_status(&ap_state);
	if (page == WIFI_LIST) {
		header("Wi-Fi", close_cb);
		button(root, "个人热点", 84, 6, 82, 28, hotspot_cb, NULL);
		lv_obj_t *card = button(root, "", 8, 40, w - 16, 44, details_cb, NULL);
		connection_name = text(card, "", 8, 1, w - 32, GREEN);
		lv_obj_set_height(connection_name, 21);
		lv_label_set_long_mode(connection_name, LV_LABEL_LONG_MODE_DOTS);
		connection_info = text(card, "", 8, 22, w - 32, MUTED);
		text(root, "附近网络", 12, 87, 92, MUTED);
		button(root, "添加", w - 102, 86, 42, 22, manual_cb, NULL);
		lv_obj_t *b = button(root, "扫描", w - 56, 86, 48, 22, scan_cb, NULL);
		scan_label = lv_obj_get_child(b, 0);
		networks = lv_obj_create(root);
		style(networks, 4, 110, w - 8, h - 114, BG);
		lv_obj_add_flag(networks, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_scroll_dir(networks, LV_DIR_VER);
		list_rows();
	} else if (page == WIFI_JOIN || page == HOTSPOT_EDIT) {
		header(page == WIFI_JOIN ? "连接网络" : "设置个人热点",
		       page == WIFI_JOIN ? wifi_cb : hotspot_cb);
		text(root, page == HOTSPOT_EDIT ? "热点名称" : "网络名称", 12, 48, w - 24, MUTED);
		ssid_field = field(72, selected, 32);
		text(root, "连接密码", 12, 116, w - 24, MUTED);
		button(root, "显示", w - 58, 112, 46, 26, reveal_cb, NULL);
		password_field = field(140, "", page == HOTSPOT_EDIT ? 63 : 64);
		lv_textarea_set_password_mode(password_field, true);
		hint = text(root,
			    page == HOTSPOT_EDIT ? (ap_state.configured ? "留空可保留原密码"
									: "请设置至少8位连接密码")
			    : require_password	 ? "请输入热点的连接密码"
						 : "开放网络的密码可留空",
			    12, 184, w - 24, MUTED);
		lv_obj_t *submit = button(root, page == WIFI_JOIN ? "连接" : "保存并开启", 12,
					  h - 48, w - 24, 36, submit_cb, NULL);
		lv_obj_set_style_bg_color(submit, lv_color_hex(ACCENT), 0);
		lv_obj_set_style_text_color(lv_obj_get_child(submit, 0), lv_color_hex(BG), 0);
		keyboard = lv_buttonmatrix_create(root);
		style(keyboard, 4, h - 124, w - 8, 120, PANEL);
		lv_obj_set_style_pad_row(keyboard, 3, 0);
		lv_obj_set_style_pad_column(keyboard, 2, 0);
		lv_obj_set_style_text_font(keyboard, aster_ui_font(16), LV_PART_ITEMS);
		lv_obj_set_style_text_color(keyboard, lv_color_hex(FG), LV_PART_ITEMS);
		lv_obj_set_style_bg_color(keyboard, lv_color_hex(RULE), LV_PART_ITEMS);
		lv_obj_set_style_border_width(keyboard, 0, LV_PART_ITEMS);
		lv_obj_set_style_shadow_width(keyboard, 0, LV_PART_ITEMS);
		lv_obj_set_style_pad_all(keyboard, 0, LV_PART_ITEMS);
		caps = numeric = symbols = false;
		lv_buttonmatrix_set_map(keyboard, letters);
		lv_obj_add_event_cb(keyboard, key_cb, LV_EVENT_VALUE_CHANGED, NULL);
		lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
	} else if (page == HOTSPOT) {
		header("个人热点", wifi_cb);
		lv_obj_t *card = lv_obj_create(root);
		style(card, 8, 44, w - 16, 56, PANEL);
		lv_obj_set_style_radius(card, 10, 0);
		text(card, "允许设备连接", 10, 7, w - 88, FG);
		hotspot_info = text(card, "", 10, 30, w - 88, MUTED);
		hotspot_switch = lv_switch_create(card);
		lv_obj_set_pos(hotspot_switch, w - 78, 16);
		lv_obj_set_size(hotspot_switch, 42, 24);
		lv_obj_set_style_bg_color(hotspot_switch, lv_color_hex(ACCENT),
					  LV_PART_INDICATOR | LV_STATE_CHECKED);
		if (ap_state.enabled)
			lv_obj_add_state(hotspot_switch, LV_STATE_CHECKED);
		lv_obj_add_event_cb(hotspot_switch, hotspot_toggle_cb, LV_EVENT_VALUE_CHANGED,
				    NULL);
		lv_obj_t *name = button(root, "", 8, 116, w - 16, 54, hotspot_edit_cb, NULL);
		text(name, "热点名称", 10, 3, w - 36, MUTED);
		hotspot_name = text(name, ap_state.ssid, 10, 28, w - 36, FG);
		button(root, "修改连接密码", 8, 180, w - 16, 42, hotspot_edit_cb, NULL);
		text(root, "本机 IP", 18, 236, w - 36, MUTED);
		text(root, "192.168.4.1", 18, 258, w - 36, FG);
		text(root, "局域网热点 · 自动分配地址", 12, h - 27, w - 24, MUTED);
	} else if (page == WIFI_FORGET_CONFIRM) {
		header("忘记网络", cancel_forget_cb);
		char saved[33] = {0};
		aster_plat_wifi_last_ssid(saved, sizeof(saved));
		text(root, saved, 12, 64, w - 24, FG);
		text(root, "将删除保存的连接密码。\n再次连接时需要重新输入。", 12, 116, w - 24,
		     MUTED);
		lv_obj_t *confirm = button(root, "确认忘记", 12, h - 92, w - 24, 36, action_cb,
					   (void *)(uintptr_t)ASTER_PLAT_WIFI_OP_FORGET);
		lv_obj_set_style_text_color(lv_obj_get_child(confirm, 0), lv_color_hex(RED), 0);
		button(root, "取消", 12, h - 48, w - 24, 36, cancel_forget_cb, NULL);
	} else {
		header("网络详情", wifi_cb);
		aster_wifi_status_t st = {0};
		aster_plat_wifi_status(&st);
		char saved[33] = {0};
		aster_plat_wifi_last_ssid(saved, sizeof(saved));
		connection_name = text(root, st.ssid[0] ? st.ssid : saved, 12, 52, w - 24, FG);
		connection_info = text(root, "", 12, 110, w - 24, MUTED);
		button(root, st.state == WIFI_STATE_COMPLETED ? "断开连接" : "重新连接", 12, h - 92,
		       w - 24, 36, action_cb,
		       (void *)(uintptr_t)(st.state == WIFI_STATE_COMPLETED
					       ? ASTER_PLAT_WIFI_OP_DISCONNECT
					       : ASTER_PLAT_WIFI_OP_RECONNECT));
		lv_obj_t *b = button(root, "忘记此网络", 12, h - 48, w - 24, 36, forget_cb, NULL);
		lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_hex(RED), 0);
	}
	aster_wifi_settings_tick();
}
static void deleted(lv_event_t *e)
{
	(void)e;
	root = NULL;
	owner_close = NULL;
	reset_view();
#if IS_ENABLED(CONFIG_ASTER_MDNS)
	(void)aster_mdns_svc_release(ASTER_MDNS_OWNER_UI);
#endif
}
bool aster_wifi_settings_is_open(void) { return root != NULL; }
void aster_wifi_settings_warm(void)
{
	const char *charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
			      "!@#$%^&*()-_+=[]{}\\|;:'\",.<>/?~` ·…"
			      "Wi-"
			      "Fi个人热点关闭返回附近网络添加扫描未连接正在获取地址连接密码名称显示"
			      "隐藏开放留空保存并开启允许设备已断开重新忘记此详情自动分配局域网完成"
			      "删除空格选择失败请检查信号至少设置八位将再次时需要输入确认取消";
	aster_ui_font_warm(16, charset);
	aster_ui_font_warm(20, "Wi-Fi连接网络网络详情个人热点设置个人热点忘记网络");
}
void aster_wifi_settings_open(lv_obj_t *parent, void (*close_owner)(void))
{
	if (root)
		return;
	lv_obj_update_layout(parent);
	w = lv_obj_get_content_width(parent);
	h = lv_obj_get_content_height(parent);
	owner_close = close_owner;
	root = lv_obj_create(parent);
	lv_obj_add_flag(root, LV_OBJ_FLAG_FLOATING);
	style(root, 0, 0, w, h, BG);
	lv_obj_set_style_radius(root, 0, 0);
	lv_obj_update_layout(root);
	lv_obj_add_event_cb(root, deleted, LV_EVENT_DELETE, NULL);
	ap_count = 0;
	pending = 0;
	aster_wifi_settings_warm();
	show(WIFI_LIST, NULL);
	scan_cb(NULL);
#if IS_ENABLED(CONFIG_ASTER_MDNS)
	/* Plan A: publishing follows the settings page, no persistent toggle. */
	(void)aster_mdns_svc_acquire(ASTER_MDNS_OWNER_UI);
#endif
}
void aster_wifi_settings_tick(void)
{
	if (!root)
		return;
	if (toast && (int32_t)(k_uptime_get_32() - toast_until) >= 0)
		lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
	uint32_t op, last;
	int err;
	aster_plat_wifi_operation(&op, &last, &err);
	if (pending && !op && last == pending) {
		uint32_t completed = pending;
		pending = 0;
		if (err)
			failed(err);
		else if (completed == ASTER_PLAT_WIFI_OP_SCAN) {
			if (!aster_plat_wifi_scan_results(aps, ASTER_WIFI_AP_MAX, &ap_count)) {
				for (int i = 0; i < ap_count; i++)
					aster_ui_font_warm(16, aps[i].ssid);
				list_rows();
			}
		} else if (completed == ASTER_PLAT_WIFI_OP_FORGET)
			notify("已忘记此网络");
	}
	set_text(scan_label, op == ASTER_PLAT_WIFI_OP_SCAN ? "扫描中" : "扫描");
	if (page == HOTSPOT) {
		aster_plat_wifi_hotspot_status(&ap_state);
		if (ap_state.enabled)
			lv_obj_add_state(hotspot_switch, LV_STATE_CHECKED);
		else
			lv_obj_clear_state(hotspot_switch, LV_STATE_CHECKED);
		set_text(hotspot_name, ap_state.ssid);
		set_text(hotspot_info, op == ASTER_PLAT_WIFI_OP_HOTSPOT_START  ? "正在开启…"
				       : op == ASTER_PLAT_WIFI_OP_HOTSPOT_STOP ? "正在关闭…"
				       : ap_state.enabled		       ? "热点已开启"
									       : "热点已关闭");
		return;
	}
	if (page != WIFI_LIST && page != WIFI_DETAILS)
		return;
	aster_wifi_status_t st = {0};
	aster_plat_wifi_status(&st);
	char saved[33] = {0}, info[160];
	aster_plat_wifi_last_ssid(saved, sizeof(saved));
	set_text(connection_name, st.state == WIFI_STATE_COMPLETED   ? st.ssid
				  : op == ASTER_PLAT_WIFI_OP_CONNECT ? "正在连接…"
								     : "未连接");
	if (page == WIFI_DETAILS) {
		set_text(connection_name, st.ssid[0] ? st.ssid : saved);
		snprintf(info, sizeof(info), "%s\n\nIP  %s\n\n安全  %s\n\n信号  %d dBm",
			 aster_ui_wifi_word(st.state), st.ip[0] ? st.ip : "未分配",
			 st.sec_txt ? st.sec_txt : "—", (int)st.rssi);
	} else
		snprintf(info, sizeof(info), "%s",
			 (st.state == WIFI_STATE_COMPLETED && st.ip[0]) ? st.ip
			 : st.state == WIFI_STATE_COMPLETED		? "正在获取 IP…"
			 : saved[0]					? saved
									: "选择网络进行连接");
	set_text(connection_info, info);
}
