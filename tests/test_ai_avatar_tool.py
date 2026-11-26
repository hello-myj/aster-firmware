"""Exercise fragmented function calls and the installed avatar's allowed clips."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
AI = ROOT / "systems/espidf/optional/aster_ai"
CJSON = ROOT / "boards/fakepod_s31/idf/managed_components/espressif__cjson/cJSON"


class AvatarTool(unittest.TestCase):
    def test_streamed_tool_requires_a_complete_supported_motion_and_speech(self):
        if not (CJSON / "cJSON.c").is_file():
            self.skipTest("ESP-IDF managed cJSON is available after configuring the FAKE POD AI build")
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "test.c"
            source.write_text(r'''
#include "avatar_tool.h"
#include <assert.h>
#include <string.h>

static void chunk(struct ai_avatar_call *call, int index, const char *name, const char *args,
                  const char *finish)
{
    cJSON *choice = cJSON_CreateObject();
    cJSON *delta = cJSON_AddObjectToObject(choice, "delta");
    if (args || name) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddItemToArray(cJSON_AddArrayToObject(delta, "tool_calls"), item);
        cJSON_AddNumberToObject(item, "index", index);
        cJSON *function = cJSON_AddObjectToObject(item, "function");
        if (name)
            cJSON_AddStringToObject(function, "name", name);
        if (args)
            cJSON_AddStringToObject(function, "arguments", args);
    }
    if (finish)
        cJSON_AddStringToObject(choice, "finish_reason", finish);
    ai_avatar_tool_collect(call, choice, delta);
    cJSON_Delete(choice);
}

int main(void)
{
    const struct ai_avatar_clip clips[] = {
        {.frames = 5, .label = "竖食指"}, {.frames = 1, .label = "圈圈眼"},
    };
    const struct ai_avatar_catalog catalog = {.clips = clips, .count = 2};
    cJSON *request = cJSON_CreateObject();
    assert(ai_avatar_tool_add(request, &catalog));
    char *schema = cJSON_PrintUnformatted(request);
    assert(strstr(schema, "\"enum\":[\"竖食指\"]"));
    assert(strstr(schema, "\"enum\":[\"圈圈眼\"]"));
    cJSON_free(schema);
    cJSON_Delete(request);

    struct ai_avatar_cue cue = {0};
    char speech[80] = {0};
    struct ai_avatar_call call = {0};
    chunk(&call, 0, "avatar_perform", "{\"motion\":\"竖", NULL);
    chunk(&call, 0, NULL, "食指\",\"speech\":\"你好", NULL);
    chunk(&call, 0, NULL, "！\",\"expression\":\"圈圈眼\"}", NULL);
    assert(!ai_avatar_tool_resolve(&call, &catalog, &cue, speech, sizeof(speech)));
    chunk(&call, 0, NULL, NULL, "tool_calls");
    assert(ai_avatar_tool_resolve(&call, &catalog, &cue, speech, sizeof(speech)));
    assert(!strcmp(cue.motion, "竖食指") && !strcmp(cue.expression, "圈圈眼"));
    assert(!strcmp(speech, "你好！"));
    strcpy(speech, "已经在播报");
    assert(ai_avatar_tool_resolve(&call, &catalog, &cue, NULL, sizeof(speech)));
    assert(!strcmp(speech, "已经在播报"));

    call = (struct ai_avatar_call){0};
    chunk(&call, 0, "avatar_perform", "{\"motion\":\"圈圈眼\",\"speech\":\"你好\"}", "tool_calls");
    assert(!ai_avatar_tool_resolve(&call, &catalog, &cue, speech, sizeof(speech)));
    call = (struct ai_avatar_call){0};
    chunk(&call, 0, "avatar_perform", "{\"motion\":\"竖食指\",\"speech\":\"你好\"}", NULL);
    chunk(&call, 1, "avatar_perform", "{}", "tool_calls");
    assert(!ai_avatar_tool_resolve(&call, &catalog, &cue, speech, sizeof(speech)));

    struct ai_avatar_catalog live = catalog;
    live.joint_mask = (UINT64_C(1) << AI_JOINT_head_yaw) | (UINT64_C(1) << AI_JOINT_left_elbow);
    request = cJSON_CreateObject();
    assert(ai_avatar_tool_add(request, &live));
    schema = cJSON_PrintUnformatted(request);
    assert(strstr(schema, "\"left_elbow\"") && !strstr(schema, "\"right_elbow\""));
    cJSON_free(schema);
    cJSON_Delete(request);

    call = (struct ai_avatar_call){0};
    chunk(&call, 0, "avatar_perform",
          "{\"joints\":{\"head_yaw\":-0.5,\"left_elbow\":0.25},\"keep_pose\":true,\"speech\":\"你好\"}", "tool_calls");
    assert(ai_avatar_tool_resolve(&call, &live, &cue, speech, sizeof(speech)));
    assert(cue.joint_mask == live.joint_mask && cue.joints[AI_JOINT_head_yaw] == -50 &&
           cue.joints[AI_JOINT_left_elbow] == 25 && cue.keep_pose);
    assert(!ai_avatar_tool_resolve(&call, &catalog, &cue, speech, sizeof(speech)));
    call = (struct ai_avatar_call){0};
    chunk(&call, 0, "avatar_perform", "{\"joints\":{\"left_elbow\":1.25},\"speech\":\"你好\"}", "tool_calls");
    assert(!ai_avatar_tool_resolve(&call, &live, &cue, speech, sizeof(speech)));
    return 0;
}
''', encoding="utf-8")
            binary = Path(tmp) / "test"
            subprocess.run([
                "cc", "-std=c11", "-I", str(CJSON), "-I", str(AI), str(source),
                str(AI / "avatar_tool.c"), str(CJSON / "cJSON.c"), "-lm", "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
