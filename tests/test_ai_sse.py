"""Verify chat SSE framing independently of HTTP packet boundaries."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class ChatEvents(unittest.TestCase):
    def test_utf8_fragments_heartbeat_and_completion(self):
        ai = Path(__file__).resolve().parents[1] / "systems/espidf/optional/aster_ai"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "test.c"
            source.write_text(r'''
#include "chat_sse.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
static int part(void *p, const char *data) {
    int *count = p;
    assert(!strcmp(data, *count ? "{\"word\":\"鲸\"}" : "{\"word\":\"小\"}"));
    ++*count;
    return 0;
}
int main(void) {
    const char wire[] = ": keepalive\r\n\r\ndata: {\"word\":\"小\"}\r\n\r\n"
                        "data:{\"word\":\"鲸\"}\n\ndata: [DONE]";
    for (size_t chunk = 1; chunk <= sizeof(wire); ++chunk) {
        int count = 0;
        struct chat_sse s = {.json = part, .context = &count};
        for (size_t pos = 0; pos < sizeof(wire)-1; ) {
            size_t n = sizeof(wire)-1-pos;
            if (n > chunk) n = chunk;
            assert(!chat_sse_feed(&s, (const uint8_t *)wire + pos, n));
            pos += n;
        }
        assert(!chat_sse_finish(&s) && count == 2);
    }
    struct chat_sse s = {0};
    assert(chat_sse_finish(&s) == -EPROTO);
    uint8_t invalid[4096];
    memset(invalid, 'a', sizeof(invalid));
    assert(chat_sse_feed(&s, invalid, sizeof(invalid)) == -EPROTO);
}
''', encoding="utf-8")
            exe = root / "sse"
            subprocess.run(["cc", "-std=c11", "-I", str(ai), str(source),
                            str(ai / "chat_sse.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
