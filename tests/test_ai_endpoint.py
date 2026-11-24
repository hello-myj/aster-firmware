"""Check the utterance limits around VADNet's debounced speech/silence events."""
import pathlib
import subprocess
import tempfile
import unittest


class SpeechEndpoint(unittest.TestCase):
    def test_wait_timeout_preroll_and_sentence_end(self):
        include = pathlib.Path(__file__).resolve().parents[1] / "systems/espidf/components/aster_ui"
        source = r'''
#include "ai_endpoint.h"
#include <assert.h>
int main(void) {
    assert(ai_endpoint_leading_skip(512) == 0);
    assert(ai_endpoint_leading_skip(AI_WAIT_LIMIT) == AI_WAIT_LIMIT - 8000);
    struct ai_endpoint g = {.captured = AI_WAIT_LIMIT - 1};
    assert(!ai_endpoint_finished(&g, false));
    ++g.captured;
    assert(ai_endpoint_finished(&g, false));
    g.heard = true;
    assert(!ai_endpoint_finished(&g, true));
    assert(ai_endpoint_finished(&g, false));
    g.captured = AI_MIC_RATE;
    assert(!ai_endpoint_finished(&g, false));
    g.captured = 2 * AI_MIC_RATE;
    assert(ai_endpoint_finished(&g, false));
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            c, exe = root / "endpoint.c", root / "endpoint"
            c.write_text(source, encoding="utf-8")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-I", str(include),
                            str(c), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
