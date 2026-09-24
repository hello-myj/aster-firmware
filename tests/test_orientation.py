"""Exercise the runtime's actual orientation lease against a fake display."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Orientation(unittest.TestCase):
    def test_app_orientation_returns_to_portrait(self):
        source = (ROOT / "systems/zephyr/runtime/aster_runtime.c").read_text()
        start = source.index("static struct {\n\tint8_t from;")
        end = source.index("int aster_runtime_init(void)", start)
        harness = r'''
#include <assert.h>
#include <stdint.h>
#define IS_ENABLED(x) 1
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)
enum { ASTER_ORIENT_ANY = -1, ASTER_ORIENT_PORTRAIT, ASTER_ORIENT_LANDSCAPE };
typedef struct { int want_orientation; const char *id; } aster_app_rec_t;
static int panel = ASTER_ORIENT_PORTRAIT, refused, writes;
static int aster_plat_display_orientation_get(void) { return panel; }
static int aster_plat_display_orientation_set(int want) {
    if (refused) return -1;
    panel = want; writes++; return 0;
}
static const char *aster_pkg_orientation_name(int8_t v) {
    return v == ASTER_ORIENT_ANY ? 0 : v == ASTER_ORIENT_PORTRAIT ? "portrait" : "landscape";
}
'''
        cases = r'''
int main(void) {
    aster_app_rec_t rec = { ASTER_ORIENT_LANDSCAPE, "test" };
    orient_lease_take(&rec);
    assert(panel == ASTER_ORIENT_LANDSCAPE);
    orient_lease_release(); /* normal exit, stop and init failure share this */
    assert(panel == ASTER_ORIENT_PORTRAIT);
    int old_writes = writes;
    rec.want_orientation = ASTER_ORIENT_PORTRAIT;
    orient_lease_take(&rec);
    orient_lease_release();
    assert(panel == ASTER_ORIENT_PORTRAIT && writes == old_writes);
    panel = ASTER_ORIENT_LANDSCAPE;
    rec.want_orientation = ASTER_ORIENT_ANY;
    orient_lease_take(&rec);
    orient_lease_release(); /* recover even without a lease */
    assert(panel == ASTER_ORIENT_PORTRAIT);
    refused = 1;
    rec.want_orientation = ASTER_ORIENT_LANDSCAPE;
    orient_lease_take(&rec);
    orient_lease_release();
    assert(panel == ASTER_ORIENT_PORTRAIT);
    panel = -1; /* no display */
    orient_lease_release();
    assert(panel == -1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "orientation")
            subprocess.run(["cc", "-std=c11", "-x", "c", "-", "-o", executable],
                           input=harness + source[start:end] + cases,
                           text=True, check=True, capture_output=True)
            subprocess.run([executable], check=True, timeout=5)
