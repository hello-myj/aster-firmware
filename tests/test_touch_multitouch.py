"""Exercise the production FT6336/AXS15231B parsers and two-slot state path."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLAT = ROOT / "platform" / "esp32s3"


class Multitouch(unittest.TestCase):
    def test_protocol_and_slot_state_under_sanitizers(self):
        code = r'''
#include "touch_protocol.h"
#include "touch_ui.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

static void axs_record(uint8_t *p, uint8_t id, uint8_t event, uint16_t x, uint16_t y) {
    p[0] = (uint8_t)((event << 6) | ((x >> 8) & 0x0f)); p[1] = (uint8_t)x;
    p[2] = (uint8_t)((id << 4) | ((y >> 8) & 0x0f)); p[3] = (uint8_t)y;
    p[4] = p[5] = 0;
}
static void ft_record(uint8_t *p, uint8_t id, uint8_t event, uint16_t x, uint16_t y) {
    p[0] = (uint8_t)((event << 6) | ((x >> 8) & 0x0f)); p[1] = (uint8_t)x;
    p[2] = (uint8_t)((id << 4) | ((y >> 8) & 0x0f)); p[3] = (uint8_t)y;
    p[4] = p[5] = 0;
}
static int drain(struct aster_touch_state *s, struct aster_touch_event *e, int n) {
    return aster_touch_state_poll(s, e, n);
}

int main(void) {
    struct aster_touch_frame f;
    /* Real Waveshare two-contact capture: Y-high nibbles carry IDs 0/1. */
    const uint8_t captured[14] = {0x00,0x02,0x81,0x17,0x00,0x72,0x5f,0x60,
                                0x81,0x27,0x11,0x71,0x60,0x61};
    assert(aster_axs15231b_decode(captured,sizeof(captured),320,480,&f)==0);
    assert(f.count==2 && f.points[0].id==0 && f.points[1].id==1);
    assert(f.points[0].x==279 && f.points[0].y==114);
    assert(f.points[1].x==295 && f.points[1].y==369);
    uint8_t axs[14] = {0};
    assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0 && f.count == 0);
    axs[1] = 1; axs_record(axs + 2, 4, 0, 319, 479);
    assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0);
    assert(f.count == 1 && f.points[0].id == 4 && f.points[0].x == 319 && f.points[0].y == 479);
    axs[1] = 2; axs_record(axs + 8, 9, 2, 0, 0);
    assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0 && f.count == 2);
    axs[1] = 3; assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == -EINVAL);
    axs[1] = 2; axs_record(axs + 8, 4, 2, 1, 1);
    assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == -EINVAL);
    axs_record(axs + 8, 9, 2, 320, 1);
    assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == -ERANGE);
    axs_record(axs + 8, 9, 2, 1, 1); axs[0] = 0xff;
    assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == -EINVAL);
    assert(aster_axs15231b_decode(axs, 13, 320, 480, &f) == -EINVAL);

    uint8_t ft[13] = {0};
    ft[0] = 1; ft_record(ft + 1, 6, 0, 20, 30);
    assert(aster_ft6336_decode(ft, sizeof(ft), 240, 320, &f) == 0);
    assert(f.count == 1 && f.points[0].id == 6 && f.points[0].x == 30 && f.points[0].y == 20);
    ft[0] = 2; ft_record(ft + 7, 11, 2, 239, 319);
    assert(aster_ft6336_decode(ft, sizeof(ft), 240, 320, &f) == 0 && f.count == 2);
    assert(f.points[1].x == 319 && f.points[1].y == 239);
    ft[0] = 3; assert(aster_ft6336_decode(ft, sizeof(ft), 240, 320, &f) == -EINVAL);
    ft[0] = 1; ft_record(ft + 1, 15, 0, 1, 1);
    assert(aster_ft6336_decode(ft, sizeof(ft), 240, 320, &f) == -EINVAL);
    ft_record(ft + 1, 6, 0, 240, 1);
    assert(aster_ft6336_decode(ft, sizeof(ft), 240, 320, &f) == -ERANGE);
    assert(aster_ft6336_decode(ft, 12, 240, 320, &f) == -EINVAL);

    struct aster_touch_state s; aster_touch_state_init(&s);
    axs[0] = 0; axs[1] = 2;
    axs_record(axs + 2, 4, 0, 10, 20); axs_record(axs + 8, 9, 0, 300, 450);
    assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0);
    assert(aster_touch_state_report_frame(&s, &f) == 0);
    struct aster_touch_event out[16];
    assert(drain(&s, out, 1) == 1 && out[0].action == 1 && out[0].slot == 0 && out[0].contact_mask == 3);
    assert(drain(&s, out, 1) == 1 && out[0].action == 1 && out[0].slot == 1 && out[0].contact_mask == 3);

    /* Record order changes and the two fingers cross: hardware IDs, not
     * report order or nearest coordinates, preserve their logical slots. */
    axs_record(axs + 2, 9, 2, 20, 30); axs_record(axs + 8, 4, 2, 290, 440);
    assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0);
    assert(aster_touch_state_report_frame(&s, &f) == 0);
    assert(drain(&s, out, 16) == 2);
    assert(out[0].slot == 1 && out[0].action == 2 && out[1].slot == 0 && out[1].action == 2);
	axs[1] = 2; axs_record(axs + 2, 4, 1, 290, 440);
	axs_record(axs + 8, 9, 2, 20, 30);
    assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0);
    assert(aster_touch_state_report_frame(&s, &f) == 0);
    assert(drain(&s, out, 1) == 1 && out[0].slot == 0 && out[0].action == 3);
    struct { uint32_t words[4]; uint32_t guard; } snapshot;
    memset(&snapshot, 0xa5, sizeof(snapshot));
    aster_touch_state_snapshot(&s, snapshot.words, 1);
    assert(snapshot.words[0] == 20 && snapshot.words[1] == 30 && snapshot.words[2] == 1 && snapshot.words[3] == 1);
    assert(snapshot.guard == 0xa5a5a5a5u);
    axs[1] = 0;
    assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0);
    assert(aster_touch_state_report_frame(&s, &f) == 0);
    assert(drain(&s, out, 1) == 1 && out[0].slot == 1 && out[0].action == 3);

    /* A reused hardware ID starts a new lifecycle after an explicit UP. */
    axs[1] = 1; axs_record(axs + 2, 3, 0, 50, 60);
    assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0);
    assert(aster_touch_state_report_frame(&s, &f) == 0);
    assert(drain(&s, out, 1) == 1 && out[0].action == 1 && out[0].slot == 0);
	 axs[1] = 1; axs_record(axs + 2, 3, 1, 50, 60);
    assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0);
	assert(aster_touch_state_report_frame(&s, &f) == 0);
	assert(drain(&s, out, 1) == 1 && out[0].action == 3);
    axs_record(axs + 2, 3, 0, 51, 61);
    assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0);
    assert(aster_touch_state_report_frame(&s, &f) == 0);
    assert(drain(&s, out, 1) == 1 && out[0].action == 1);

    /* One transient read error retains contacts; bounded failure releases
     * both and ignores stale contacts until an all-up frame. */
    aster_touch_state_init(&s);
    axs[1] = 1; axs_record(axs + 2, 2, 0, 80, 90);
    assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0);
    assert(aster_touch_state_report_frame(&s, &f) == 0);
    drain(&s, out, 16);
    aster_touch_state_report_error(&s, 1000);
    uint32_t one[8]; aster_touch_state_snapshot(&s, one, 1);
    assert(one[2] == 1);
    for (int i = 1; i < 5; i++) aster_touch_state_report_error(&s, 1000 + i * 20);
    assert(drain(&s, out, 16) == 2 && out[0].action == 3 && out[1].action == 3);
    axs[1] = 1; axs_record(axs + 2, 2, 2, 100, 110);
    assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0);
    assert(aster_touch_state_report_frame(&s, &f) == 0 && drain(&s, out, 16) == 0);
    axs[1] = 0; assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0);
    assert(aster_touch_state_report_frame(&s, &f) == 0);
    axs[1] = 1; axs_record(axs + 2, 2, 0, 100, 110);
    assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0);
    assert(aster_touch_state_report_frame(&s, &f) == 0 && drain(&s, out, 16) == 1 && out[0].action == 1);

    /* Recovery also accepts an authoritative explicit-UP frame. It must
     * still reject a frame retaining another active/unknown contact. */
    for (int i=0;i<5;i++) aster_touch_state_report_error(&s,2000+i*20);
    drain(&s,out,16);
    assert(s.ignore_until_clear);
    axs[1]=2;
    axs_record(axs+2,2,1,100,110); axs_record(axs+8,3,2,90,90);
    assert(aster_axs15231b_decode(axs,sizeof(axs),320,480,&f)==0);
    aster_touch_state_report_frame(&s,&f); assert(s.ignore_until_clear);
    axs_record(axs+8,3,3,90,90);
    assert(aster_axs15231b_decode(axs,sizeof(axs),320,480,&f)==0);
    aster_touch_state_report_frame(&s,&f); assert(s.ignore_until_clear);
    axs_record(axs+8,3,1,90,90);
    assert(aster_axs15231b_decode(axs,sizeof(axs),320,480,&f)==0);
    aster_touch_state_report_frame(&s,&f); assert(!s.ignore_until_clear);
    axs[1]=1; axs_record(axs+2,2,0,100,110);
    assert(aster_axs15231b_decode(axs,sizeof(axs),320,480,&f)==0);
    aster_touch_state_report_frame(&s,&f);
    assert(drain(&s,out,16)==1 && out[0].action==1);

    /* Lost UP + unreadable idle must not suppress every future new DOWN.
     * Held CONTACT is still quarantined; an explicit new DOWN starts an
     * identifiable new contact epoch, even without an intervening zero frame. */
    aster_touch_state_init(&s);
    axs[1]=1;axs_record(axs+2,0,0,80,90);
    assert(aster_axs15231b_decode(axs,sizeof(axs),320,480,&f)==0);
    aster_touch_state_report_frame(&s,&f);drain(&s,out,16);
    for(int i=0;i<6;i++) aster_touch_state_report_error(&s,4000+i*20);
    drain(&s,out,16);assert(s.ignore_until_clear);
    axs_record(axs+2,0,2,81,91);
    assert(aster_axs15231b_decode(axs,sizeof(axs),320,480,&f)==0);
    aster_touch_state_report_frame(&s,&f);assert(s.ignore_until_clear);
    axs_record(axs+2,0,0,82,92);
    assert(aster_axs15231b_decode(axs,sizeof(axs),320,480,&f)==0);
    aster_touch_state_report_frame(&s,&f);
    assert(!s.ignore_until_clear);
    assert(drain(&s,out,16)==1 && out[0].action==1);

    /* A fresh finger beside an old held contact is not a new epoch. Both
     * active records must explicitly start anew before recovery reopens. */
    for(int i=0;i<6;i++) aster_touch_state_report_error(&s,5000+i*20);
    drain(&s,out,16);assert(s.ignore_until_clear);
    axs[1]=2;axs_record(axs+2,0,0,100,110);axs_record(axs+8,1,2,150,160);
    assert(aster_axs15231b_decode(axs,sizeof(axs),320,480,&f)==0);
    aster_touch_state_report_frame(&s,&f);assert(s.ignore_until_clear);
    axs_record(axs+8,1,0,150,160);
    assert(aster_axs15231b_decode(axs,sizeof(axs),320,480,&f)==0);
    aster_touch_state_report_frame(&s,&f);assert(!s.ignore_until_clear);
    assert(drain(&s,out,16)==2 && out[0].action==1 && out[1].action==1);

    /* Queue overflow fails closed with releases and recovers after all-up. */
    aster_touch_state_init(&s);
	for (int i = 0; i < 8; i++) {
        axs[1] = 1; axs_record(axs + 2, (uint8_t)(i % 4), 0, 10, 10);
        assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0);
        aster_touch_state_report_frame(&s, &f);
        axs[1] = 0; assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0);
        aster_touch_state_report_frame(&s, &f);
	}
	axs[1] = 1; axs_record(axs + 2, 10, 0, 20, 20);
	assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0);
	aster_touch_state_report_frame(&s, &f);
    assert(s.ignore_until_clear && !s.slots[0].down && !s.slots[1].down);
    assert(drain(&s, out, 16) == 2 && out[0].action == 3 && out[1].action == 3);
	axs[1] = 0; assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0);
	assert(aster_touch_state_report_frame(&s, &f) == 0 && !s.ignore_until_clear);

    /* Synthetic contacts survive empty hardware frames and can still be
     * moved and released by the debug injector. */
    aster_touch_state_init(&s);
    aster_touch_state_inject(&s, 0, 1, 2, 1);
    assert(drain(&s, out, 1) == 1 && out[0].action == 1);
    axs[1] = 0; assert(aster_axs15231b_decode(axs, sizeof(axs), 320, 480, &f) == 0);
    for (int i = 0; i < 5; ++i) {
        assert(aster_touch_state_report_frame(&s, &f) == 0);
        assert(drain(&s, out, 16) == 0);
    }
    uint32_t synthetic[8]; aster_touch_state_snapshot(&s, synthetic, 1);
    assert(synthetic[2] == 1 && synthetic[0] == 1 && synthetic[1] == 2);
    aster_touch_state_inject(&s, 0, 9, 10, 2);
    assert(drain(&s, out, 1) == 1 && out[0].action == 2 && out[0].x == 9);
    aster_touch_state_inject(&s, 0, 9, 10, 3);
    assert(drain(&s, out, 1) == 1 && out[0].action == 3);

    /* Two live records fill the exact snapshot extent without a terminator. */
    aster_touch_state_init(&s);
    aster_touch_state_inject(&s, 0, 11, 12, 1);
    aster_touch_state_inject(&s, 1, 21, 22, 1);
    drain(&s,out,16);
    for (int i=0;i<20;i++) aster_touch_state_report_error(&s,3000+i*20);
    assert(drain(&s,out,16)==0 && !s.ignore_until_clear);
    struct { uint32_t words[8]; uint32_t guard; } two;
    memset(&two, 0xa5, sizeof(two));
    aster_touch_state_snapshot(&s, two.words, 2);
    assert(two.words[0] == 11 && two.words[1] == 12 && two.words[2] == 1 && two.words[3] == 0);
    assert(two.words[4] == 21 && two.words[5] == 22 && two.words[6] == 1 && two.words[7] == 1);
    assert(two.guard == 0xa5a5a5a5u);
    uint32_t zero_guard = 0x12345678;
    aster_touch_state_snapshot(&s, &zero_guard, 0);
    assert(zero_guard == 0x12345678);

    /* Pre-scanning a delayed event batch quarantines the first DOWN too. */
    struct aster_touch_ui_state ui; aster_touch_ui_reset(&ui);
    uint32_t masks[] = {1, 3, 2, 0};
    uint32_t actions[] = {1, 1, 3, 3}, slots[] = {0, 1, 0, 1};
    aster_touch_ui_begin_batch(&ui, masks, 4);
    for (uint8_t i = 0; i < 4; ++i)
        assert(!aster_touch_ui_accept(&ui, i, actions[i], slots[i], masks[i]).route);
    uint32_t single_mask[] = {1};
    aster_touch_ui_begin_batch(&ui, single_mask, 1);
    assert(aster_touch_ui_accept(&ui, 0, 1, 0, 1).route);

    /* A delayed second DOWN releases the existing UI pointer once and the
     * remaining finger cannot inherit it. */
    uint32_t delayed_masks[] = {1, 3, 2, 0};
    aster_touch_ui_begin_batch(&ui, delayed_masks, 4);
    struct aster_touch_ui_decision decision = aster_touch_ui_accept(&ui, 0, 2, 0, 1);
    assert(!decision.route && decision.release_pointer && decision.reset_gesture);
    for (uint8_t i = 1; i < 4; ++i)
        assert(!aster_touch_ui_accept(&ui, i, actions[i], slots[i], delayed_masks[i]).route);
    aster_touch_ui_begin_batch(&ui, single_mask, 1);
    assert(aster_touch_ui_accept(&ui, 0, 1, 1, 2).route);

    aster_touch_ui_reset(&ui);
    assert(aster_touch_ui_accept(&ui, 0, 1, 0, 1).route);
    assert(aster_touch_ui_accept(&ui, 0, 3, 0, 2).route);
    assert(!aster_touch_ui_accept(&ui, 0, 2, 1, 2).route);
    assert(!aster_touch_ui_accept(&ui, 0, 3, 1, 0).route);
    assert(aster_touch_ui_accept(&ui, 0, 1, 1, 2).route);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "touch-test"
            result = subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(PLAT), "-I", str(ROOT / "contract" / "include"), "-x", "c", "-", "-x", "none",
                 str(PLAT / "touch_state.c"), str(PLAT / "touch_protocol.c"), str(PLAT / "touch_ui.c"),
                 "-o", str(exe)],
                input=code, text=True, capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            run = subprocess.run([str(exe)], text=True, capture_output=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
