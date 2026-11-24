"""TTS delivery and the output clock can fragment at different boundaries."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
UI = ROOT / "systems/espidf/components/aster_ui"


class ReplyAudio(unittest.TestCase):
    def test_fragmented_network_partial_device_writes_and_cancel(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "freertos").mkdir()
            headers = {
                "esp_heap_caps.h": "#include <stdlib.h>\n#define heap_caps_malloc(n,c) malloc(n)",
                "esp_timer.h": "#include <stdint.h>\nstatic int64_t esp_timer_get_time(void){ return 1; }",
                "esp_log.h": "#include <stdio.h>\n#define ESP_LOGI(t,...) printf(__VA_ARGS__)",
                "freertos/FreeRTOS.h": "",
                "freertos/task.h": "static void vTaskDelay(int ticks){(void)ticks;}",
            }
            for name, data in headers.items():
                (root / name).write_text(data + "\n")
            test = root / "audio.c"
            test.write_text(r'''
#include "ai_reply_audio.h"
#include "aster_audio_output.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
static size_t frames;
static int opens, drains, closes;
static bool abort_now;
static bool cancelled(void) { return abort_now; }
static int refill(void *context, struct ai_pcm_block **block) {
    struct ai_reply_audio *pending = context;
    *block = pending->first;
    if (*block) {
        pending->first = (*block)->next;
        (*block)->next = NULL;
    }
    return 0;
}
static int sample(size_t i) { return (i & 1) ? -32768 : 32767; }
int aster_audio_output_caps(uint32_t id, aster_audio_output_caps_t *caps) {
    caps->max_write_frames = 119;
    return 0;
}
int aster_audio_output_open(uint32_t id, const aster_audio_format_t *f) {
    ++opens;
    assert(f->rate == 48000 && f->channels == 2 && f->format == ASTER_AUDIO_PCM_S32LE);
    return 0;
}
int aster_audio_output_write(uint32_t id, const void *pcm, uint32_t n) {
    const int32_t *out = pcm;
    assert(n > 0 && n <= 119);
    if (n > 37) n = 37;
    for (unsigned i = 0; i < n; ++i, ++frames) {
        size_t p = frames / 2;
        int current = sample(p), previous = p ? sample(p - 1) : current;
        int expected = frames % 2 ? current * 65536 : (previous + current) * 32768;
        assert(out[2*i] == expected && out[2*i+1] == expected);
    }
    return n;
}
int aster_audio_output_drain(uint32_t id) { ++drains; return 0; }
int aster_audio_output_close(uint32_t id) { ++closes; return 0; }
int main(void) {
    uint8_t src[33002];
    struct ai_reply_audio audio = {0};
    for (size_t i = 0; i < sizeof(src); i += 2) {
        uint16_t v = (uint16_t)sample(i/2);
        src[i] = v; src[i+1] = v >> 8;
    }
    for (size_t pos = 0; pos < sizeof(src);) {
        size_t n = sizeof(src)-pos > 213 ? 213 : sizeof(src)-pos;
        assert(!ai_reply_audio_append(&audio, src+pos, n));
        pos += n;
    }
    assert(!opens && !frames);
    /* The writer starts with two prebuffered blocks and obtains the tail
       from the producer after playback starts. */
    struct ai_reply_audio tail = {.first = audio.last};
    audio.last = audio.first->next;
    audio.last->next = NULL;
    audio.bytes -= tail.first->used;
    audio.refill = refill;
    audio.context = &tail;
    struct ai_reply_sink sink = {0};
    assert(!ai_reply_sink_open(&sink));
    assert(!ai_reply_audio_play(&audio, &sink, cancelled));
    assert(frames == sizeof(src) && opens == 1 && drains == 1 && closes == 0);
    assert(!tail.first);
    abort_now = true;
    assert(ai_reply_audio_play(&audio, &sink, cancelled) == -ECANCELED && drains == 1);
    abort_now = false;
    assert(ai_reply_audio_append(&audio, src, 1) == 0);
    assert(ai_reply_audio_play(&audio, &sink, cancelled) == -EPROTO);
    assert(!ai_reply_sink_close(&sink) && closes == 1);
    assert(!ai_reply_sink_close(&sink) && closes == 1);
    ai_reply_audio_clear(&audio);
    assert(!audio.first && !audio.last && !audio.bytes);
    return 0;
}
''', encoding="utf-8")
            exe = root / "audio"
            subprocess.run(["cc", "-std=c11", "-I", str(root), "-I", str(UI),
                            "-I", str(ROOT / "common/audio"), str(test),
                            str(UI / "ai_reply_audio.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, stdout=subprocess.DEVNULL)
