"""Exercise launcher callbacks without a device: deletion requires confirmation."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    start = source.index("static void " + name + "(")
    opening = source.index("\n{", start) + 1
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


class LauncherUninstall(unittest.TestCase):
    def test_confirmation_identity_and_failure(self):
        source = (ROOT / "systems/zephyr/ui/launcher/launcher.c").read_text()
        callbacks = "\n".join(function(source, name) for name in (
            "on_click", "uninstall_close", "on_uninstall_action",
            "on_app_tile", "uninstall_pump"))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define LOG_INF(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LAUNCHER_N_THEMES 5
enum { ASTER_OK, ASTER_EBUSY = -5, ASTER_ENOENT = -6 };
enum { UNINSTALL_NONE, UNINSTALL_CANCEL, UNINSTALL_CONFIRM };
enum { LV_EVENT_SHORT_CLICKED, LV_EVENT_LONG_PRESSED, LV_EVENT_CLICKED };
enum { LV_OBJ_FLAG_HIDDEN = 1 };
typedef int lv_event_code_t;
typedef struct { int hidden; } lv_obj_t;
typedef struct { int code; intptr_t data; } lv_event_t;
typedef struct { char id[64], name[40]; } aster_app_rec_t;
static aster_app_rec_t record = { "test.one", "First app" };
static char s_apps[2][64] = { "test.one", "test.two" };
static int s_app_n = 2, s_theme, s_page, s_visible = 1;
static bool s_dirty, s_uninstall_show;
static int s_uninstall_action, launches, deletions, result, waits;
static char s_uninstall_id[64], s_uninstall_name[40], deleted_id[64];
static lv_obj_t modal, message, confirm;
static lv_obj_t *s_uninstall_modal, *s_uninstall_message, *s_uninstall_confirm;
static void *lv_event_get_user_data(lv_event_t *e) { return (void *)e->data; }
static int lv_event_get_code(lv_event_t *e) { return e->code; }
static void persist(void) {}
static void go_page(int p) { s_page = p; }
static void aster_gesture_reset(void) {}
static void *aster_ui_lvgl_indev(void) { return 0; }
static void lv_indev_wait_release(void *p) { (void)p; waits++; }
static void lv_obj_delete(lv_obj_t *p) { (void)p; }
static void lv_label_set_text(lv_obj_t *p, const char *s) { (void)p; assert(*s); }
static void aster_ui_font_warm(int px, const char *s) { (void)px; (void)s; }
static void lv_obj_add_flag(lv_obj_t *p, int f) { p->hidden |= f; }
static int aster_runtime_launch_async(const char *id) {
    assert(strcmp(id, "test.one") == 0); launches++; return 0;
}
static const aster_app_rec_t *aster_app_mgr_find(const char *id) {
    return strcmp(id, record.id) == 0 ? &record : 0;
}
static int aster_app_mgr_uninstall(const char *id) {
    deletions++; snprintf(deleted_id, sizeof(deleted_id), "%s", id);
    if (!result) s_dirty = true;
    return result;
}
static void uninstall_open(void) {
    s_uninstall_show = false;
    s_uninstall_modal = &modal; s_uninstall_message = &message;
    s_uninstall_confirm = &confirm; confirm.hidden = 0;
}
'''
        cases = r'''
int main(void) {
    lv_event_t tile = { LV_EVENT_SHORT_CLICKED, 100 };
    on_app_tile(&tile);
    assert(launches == 1 && deletions == 0); /* ordinary tap still launches */
    tile.code = LV_EVENT_LONG_PRESSED;
    on_app_tile(&tile);
    assert(waits == 1 && s_uninstall_show && deletions == 0);
    tile.code = LV_EVENT_CLICKED;
    on_app_tile(&tile); /* long-press release cannot also launch */
    assert(launches == 1);
    uninstall_pump();
    lv_event_t cancel = { LV_EVENT_CLICKED, UNINSTALL_CANCEL };
    on_uninstall_action(&cancel);
    assert(s_uninstall_modal && deletions == 0); /* callback doesn't delete tree */
    uninstall_pump();
    assert(!s_uninstall_modal && deletions == 0);

    tile.code = LV_EVENT_LONG_PRESSED;
    on_app_tile(&tile);
    /* An external scan/compaction changes slots while the prompt is open. */
    strcpy(record.id, "test.two");
    strcpy(s_apps[0], "test.two");
    uninstall_pump();
    lv_event_t accept = { LV_EVENT_CLICKED, UNINSTALL_CONFIRM };
    on_uninstall_action(&accept);
    assert(deletions == 0); /* filesystem work is deferred */
    uninstall_pump();
    assert(deletions == 1 && strcmp(deleted_id, "test.one") == 0);
    assert(!s_uninstall_modal && s_dirty);
    uninstall_pump();
    assert(deletions == 1); /* exactly one request */

    tile.code = LV_EVENT_LONG_PRESSED;
    on_app_tile(&tile);
    uninstall_pump();
    result = ASTER_EBUSY;
    on_uninstall_action(&accept);
    uninstall_pump();
    assert(deletions == 2 && s_uninstall_modal && confirm.hidden);
    on_uninstall_action(&cancel);
    uninstall_pump();
    assert(!s_uninstall_modal && !s_uninstall_id[0]);
    strcpy(s_apps[0], "missing");
    on_app_tile(&tile);
    uninstall_pump();
    assert(!s_uninstall_modal && deletions == 2); /* stale icon cannot delete */
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "launcher-uninstall")
            compiled = subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-x", "c", "-", "-o", executable],
                input=harness + callbacks + cases, text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            subprocess.run([executable], check=True, timeout=5)
