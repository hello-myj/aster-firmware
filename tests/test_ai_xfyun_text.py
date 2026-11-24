"""Filter transcription markers without removing ordinary brackets."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class XfyunText(unittest.TestCase):
    def test_markers_and_regular_brackets(self):
        root = Path(__file__).resolve().parents[1] / "systems/espidf/optional/aster_ai"
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / "test.c"
            test.write_text(r'''
#include "xfyun_text.h"
#include <assert.h>
int main(void) {
    char only[] = " [E]  [E] ";
    assert(xfyun_strip_markers(only) == 0);
    char query[] = "  [E]  你好 [E] 小鲸[B] [E] ";
    assert(xfyun_strip_markers(query) == strlen("你好 小鲸[B]"));
    assert(!strcmp(query, "你好 小鲸[B]"));
    char unfinished[] = "[EF] [E";
    xfyun_strip_markers(unfinished);
    assert(!strcmp(unfinished, "[EF] [E"));
    return 0;
}
''', encoding="utf-8")
            executable = test.with_suffix("")
            subprocess.run(["cc", "-std=c11", "-I", str(root), str(test), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_sender_drains_in_order_and_sends_final_frame_once(self):
        root = Path(__file__).resolve().parents[1] / "systems/espidf/optional/aster_ai"
        source = (root / "xfyun_asr.c").read_text(encoding="utf-8")
        start = source.index("static int drain_input(")
        end = source.index("\nstatic void sender_task(", start)
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / "sender.c"
            test.write_text(r'''
#include "xfyun_asr.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#define FRAME_SAMPLES 512
typedef unsigned TickType_t;
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
struct xfyun_asr {
    struct { atomic_int error; atomic_bool connected; } socket;
    atomic_bool sealed, abort_sender;
    atomic_size_t available;
    size_t sent;
    int64_t next_frame;
    const int16_t *input;
    struct xfyun_asr *input_changed;
};
static unsigned data_frames, end_frames, wakes;
static int64_t now;
static int64_t esp_timer_get_time(void) { return now; }
static int xSemaphoreTake(struct xfyun_asr *a, TickType_t delay)
{
    ++wakes;
    now += delay * 1000;
    if (wakes == 1)
        atomic_store(&a->available, 1024);
    if (wakes == 2) {
        atomic_store(&a->available, 1027);
        atomic_store(&a->sealed, true);
    }
    return 0;
}
static int send_end(struct xfyun_asr *a)
{
    assert(a->sent == 1027 && data_frames == 3);
    ++end_frames;
    return 0;
}
static int send_audio(struct xfyun_asr *a, const int16_t *pcm, size_t count, bool last)
{
    assert(!last && pcm == a->input + a->sent);
    assert(count == (data_frames < 2 ? 512 : 3));
    a->sent += count;
    a->next_frame = now + 26000;
    ++data_frames;
    return 0;
}
''' + source[start:end] + r'''
int main(void)
{
    const int16_t pcm[1027] = {0};
    struct xfyun_asr a = {0};
    a.input = pcm;
    a.input_changed = &a;
    atomic_store(&a.socket.connected, true);
    atomic_store(&a.available, 512);
    assert(drain_input(&a) == 0);
    assert(a.sent == 1027 && data_frames == 3 && end_frames == 1);
    atomic_store(&a.abort_sender, true);
    assert(drain_input(&a) == -ECANCELED && end_frames == 1);
    atomic_store(&a.abort_sender, false);
    atomic_store(&a.socket.error, -ECONNRESET);
    assert(drain_input(&a) == -ECONNRESET && end_frames == 1);
    return 0;
}
''', encoding="utf-8")
            executable = test.with_suffix("")
            subprocess.run(["cc", "-std=c11", "-I", str(root), str(test), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_reconnect_replays_complete_utterance_once(self):
        root = Path(__file__).resolve().parents[1] / "systems/espidf/optional/aster_ai"
        source = (root / "xfyun_asr.c").read_text(encoding="utf-8")
        start = source.index("int xfyun_asr_transcribe(")
        end = source.index("\nvoid xfyun_asr_close(", start)
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / "retry.c"
            test.write_text(r'''
#include "xfyun_asr.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
#define ESP_LOGW(...) ((void)0)
struct xfyun_asr { int unused; };
static struct xfyun_asr old, fresh;
static const int16_t audio[5000];
static int calls, opened, closed, original_rc, replay_rc;
static bool stopped;
static bool cancelled(void) { return stopped; }
int xfyun_asr_start(struct xfyun_asr **out, const struct ai_speech_keys *keys)
{
    (void)keys;
    assert(closed == 1);
    ++opened;
    *out = &fresh;
    return 0;
}
void xfyun_asr_close(struct xfyun_asr *a)
{
    assert(a == &old);
    ++closed;
}
static int finish_audio(struct xfyun_asr *a, const int16_t *pcm, size_t n, bool replay,
                        char *question, size_t capacity, bool (*cancel)(void))
{
    ++calls;
    assert(pcm == audio && n == 5000 && !cancel() && capacity >= 3);
    assert(a == (replay ? &fresh : &old));
    int rc = replay ? replay_rc : original_rc;
    if (!rc)
        strcpy(question, "ok");
    return rc;
}
''' + source[start:end] + r'''
int main(void)
{
    struct ai_speech_keys keys = {0};
    struct xfyun_asr *a = &old;
    char question[16] = {0};
#define TRANSCRIBE(err) xfyun_asr_transcribe(&a, &keys, err, audio, 5000, question, sizeof(question), cancelled)
    assert(TRANSCRIBE(-ECONNRESET) == 0);
    assert(opened == 1 && closed == 1 && calls == 1 && a == &fresh && !strcmp(question, "ok"));
    opened = closed = calls = 0;
    a = &old;
    original_rc = -ETIMEDOUT;
    replay_rc = -ECONNRESET;
    assert(TRANSCRIBE(0) == -ECONNRESET);
    assert(opened == 1 && closed == 1 && calls == 2 && a == &fresh);
    opened = closed = calls = 0;
    a = &old;
    assert(TRANSCRIBE(-EACCES) == -EACCES);
    stopped = true;
    assert(TRANSCRIBE(-ECONNRESET) == -ECANCELED);
    assert(!opened && !closed && !calls && a == &old);
    return 0;
}
''', encoding="utf-8")
            executable = test.with_suffix("")
            subprocess.run(["cc", "-std=c11", "-I", str(root), str(test), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
