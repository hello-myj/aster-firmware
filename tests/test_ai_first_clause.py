"""The first TTS commit may land in the middle of an SSE delta."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class SpeechClause(unittest.TestCase):
    def test_first_boundary_and_exact_tail(self):
        include = Path(__file__).resolve().parents[1] / "systems/espidf/optional/aster_ai"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "test.c"
            source.write_text(r'''
#include "speech_clause.h"
#include <assert.h>
int main(void) {
    const char *s = "夜里，小鲸游到了港口。它望向远处。";
    size_t end = strlen("夜里，小鲸游到了港口。");
    assert(!speech_first_clause(s, end - 1));
    assert(speech_first_clause(s, strlen(s)) == end);
    assert(!strcmp(s+end, "它望向远处。"));
    const char *number = "The wavelength is 1.25 meters. Next sentence.";
    assert(!strcmp(number + speech_first_clause(number, strlen(number)), " Next sentence."));
    const char *longer = "她穿过了雾里的那片树林，听见了海浪。";
    assert(!strcmp(longer + speech_first_clause(longer, strlen(longer)), "听见了海浪。"));
    assert(speech_first_clause("二。", strlen("二。")) == strlen("二。"));
    assert(!speech_has_words("”）\n"));
    assert(speech_has_words("”\n她抬起头。"));
    return 0;
}
''', encoding='utf-8')
            exe = root / "clause"
            subprocess.run(["cc", "-std=c11", "-I", str(include), str(source), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
