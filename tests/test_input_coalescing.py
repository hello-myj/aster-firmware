"""Real touch-state queue keeps the origin and release of a delayed swipe."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLAT = ROOT / "platform" / "esp32s3"


class InputCoalescing(unittest.TestCase):
    def test_press_origin_and_release_survive_delayed_poll(self):
        code = r'''
#include "touch_state.h"
#include <assert.h>
#include <string.h>

static void report(struct aster_touch_state *s, unsigned id, unsigned x,
                   unsigned y, unsigned active) {
    struct aster_touch_frame f = {0};
    if (active) {
        f.count = 1;
        f.points[0] = (struct aster_touch_sample){.x=x, .y=y, .id=id,
                                               .state=ASTER_TOUCH_SAMPLE_ACTIVE};
    }
    assert(aster_touch_state_report_frame(s, &f) == 0);
}

int main(void) {
    struct aster_touch_state s;
    struct aster_touch_event out[2];
    aster_touch_state_init(&s);

    /* Device reports press then moves before the APP reaches its next frame. */
    report(&s, 3, 100, 4, 1);
    report(&s, 3, 100, 180, 1);
    assert(aster_touch_state_poll(&s, out, 2) == 2);
    assert(out[0].action == 1 && out[0].slot == 0 && out[0].y == 4);
    assert(out[1].action == 2 && out[1].slot == 0 && out[1].y == 180);
    report(&s, 3, 0, 0, 0);
    assert(aster_touch_state_poll(&s, out, 2) == 1);
    assert(out[0].action == 3 && out[0].slot == 0 && out[0].y == 180);

    /* A whole fast swipe arrives between frames; final movement precedes UP. */
    report(&s, 5, 100, 475, 1);
    report(&s, 5, 100, 260, 1);
    report(&s, 5, 0, 0, 0);
    assert(aster_touch_state_poll(&s, out, 2) == 2);
    assert(out[0].action == 1 && out[0].y == 475);
    assert(out[1].action == 2 && out[1].y == 260);
    assert(aster_touch_state_poll(&s, out, 1) == 1);
    assert(out[0].action == 3 && out[0].y == 260);

    /* A one-event consumer must not lose the pending movement or release. */
    report(&s, 7, 90, 3, 1);
    report(&s, 7, 90, 190, 1);
    report(&s, 7, 0, 0, 0);
    assert(aster_touch_state_poll(&s, out, 1) == 1 && out[0].action == 1 && out[0].y == 3);
    assert(aster_touch_state_poll(&s, out, 1) == 1 && out[0].action == 2 && out[0].y == 190);
    assert(aster_touch_state_poll(&s, out, 1) == 1 && out[0].action == 3 && out[0].y == 190);
    assert(aster_touch_state_poll(&s, out, 1) == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "input-coalescing"
            result = subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(PLAT), "-x", "c", "-", "-x", "none",
                 str(PLAT / "touch_state.c"), "-o", str(exe)],
                input=code, text=True, capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            run = subprocess.run([str(exe)], text=True, capture_output=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
