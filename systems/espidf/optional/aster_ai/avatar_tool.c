/* SPDX-License-Identifier: MIT */
#include "avatar_tool.h"
#include <math.h>
#include <string.h>

static const char tool_name[] = "avatar_perform";
#define JOINT_LABEL(name, id) [AI_JOINT_##name] = #name,
static const char *const joint_names[] = {ASTER_RIG_JOINTS(JOINT_LABEL)};
#undef JOINT_LABEL

static bool add_joints(cJSON *properties, uint64_t supported)
{
    if (!supported)
        return true;
    cJSON *joints = cJSON_AddObjectToObject(properties, "joints");
    cJSON *channels = cJSON_AddObjectToObject(joints, "properties");
    cJSON *keep = cJSON_AddObjectToObject(properties, "keep_pose");
    if (!channels || !cJSON_AddStringToObject(joints, "type", "object") ||
        !cJSON_AddBoolToObject(joints, "additionalProperties", false) ||
        !cJSON_AddStringToObject(keep, "type", "boolean"))
        return false;
    for (unsigned i = 0; i < AI_JOINT_COUNT; i++) {
        if (!(supported & (UINT64_C(1) << i)))
            continue;
        cJSON *field = cJSON_AddObjectToObject(channels, joint_names[i]);
        if (!cJSON_AddStringToObject(field, "type", "number") ||
            !cJSON_AddNumberToObject(field, "minimum", -1) || !cJSON_AddNumberToObject(field, "maximum", 1))
            return false;
    }
    return true;
}

static bool add_enum(cJSON *properties, const char *name, const struct ai_avatar_catalog *catalog,
                     bool motion)
{
    cJSON *values = NULL;
    for (unsigned i = 0; i < catalog->count; i++) {
        const struct ai_avatar_clip *clip = &catalog->clips[i];
        if ((clip->frames > 1) != motion)
            continue;
        if (!values) {
            cJSON *field = cJSON_AddObjectToObject(properties, name);
            if (!cJSON_AddStringToObject(field, "type", "string"))
                return false;
            values = cJSON_AddArrayToObject(field, "enum");
            if (!values)
                return false;
        }
        cJSON *label = cJSON_CreateString(clip->label);
        if (!cJSON_AddItemToArray(values, label)) {
            cJSON_Delete(label);
            return false;
        }
    }
    return true;
}

bool ai_avatar_tool_add(cJSON *request, const struct ai_avatar_catalog *catalog)
{
    cJSON *tools = cJSON_AddArrayToObject(request, "tools");
    cJSON *tool = cJSON_CreateObject();
    if (!cJSON_AddItemToArray(tools, tool)) {
        cJSON_Delete(tool);
        return false;
    }
    cJSON *function = cJSON_AddObjectToObject(tool, "function");
    cJSON *parameters = cJSON_AddObjectToObject(function, "parameters");
    cJSON *properties = cJSON_AddObjectToObject(parameters, "properties");
    cJSON *speech = cJSON_AddObjectToObject(properties, "speech");
    cJSON *required = cJSON_AddArrayToObject(parameters, "required");
    cJSON *field = cJSON_CreateString("speech");
    if (!cJSON_AddItemToArray(required, field)) {
        cJSON_Delete(field);
        return false;
    }
    return cJSON_AddStringToObject(tool, "type", "function") &&
           cJSON_AddStringToObject(function, "name", tool_name) &&
           cJSON_AddStringToObject(function, "description",
               catalog->joint_mask ? "让小鲸实时表演。speech 是全部台词。可以独立组合 joints、motion、expression。"
               "joints 从 -1 到 1，0 为默认姿态。手臂自然举起使用小幅正数。keep_pose 仅在用户明确要求保持时设 true。" :
               "让小鲸在本轮播报时表演。speech 是要读出的完整台词。motion 在起播时播放。"
               "expression 单独使用时从起播显示，有 motion 时等动作播完再显示。"
               "省略 expression 时持续播放所选动作，直到下一次表演。"
               "必须给 motion 或 expression 中至少一个。") &&
           cJSON_AddStringToObject(parameters, "type", "object") &&
           cJSON_AddBoolToObject(parameters, "additionalProperties", false) &&
           cJSON_AddStringToObject(speech, "type", "string") &&
           add_enum(properties, "motion", catalog, true) &&
           add_enum(properties, "expression", catalog, false) && add_joints(properties, catalog->joint_mask);
}

void ai_avatar_tool_collect(struct ai_avatar_call *call, const cJSON *choice, const cJSON *delta)
{
    const char *reason = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(choice, "finish_reason"));
    if (reason && !strcmp(reason, "tool_calls"))
        call->finished = true;
    const cJSON *calls = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
    if (!calls)
        return;
    call->seen = true;
    if (!cJSON_IsArray(calls)) {
        call->invalid = true;
        return;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, calls) {
        const cJSON *index = cJSON_GetObjectItemCaseSensitive(item, "index");
        const cJSON *function = cJSON_GetObjectItemCaseSensitive(item, "function");
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(function, "name"));
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(function, "arguments");
        if (!cJSON_IsNumber(index) || index->valuedouble != 0 || !cJSON_IsObject(function) ||
            (name && (call->named || strcmp(name, tool_name))) || (args && !cJSON_IsString(args))) {
            call->invalid = true;
            return;
        }
        if (name)
            call->named = true;
        const char *text = cJSON_GetStringValue(args);
        if (text) {
            size_t length = strlen(text);
            if (length >= sizeof(call->arguments) - call->used) {
                call->invalid = true;
                return;
            }
            memcpy(call->arguments + call->used, text, length + 1);
            call->used += length;
        }
    }
}

static bool read_label(const cJSON *value, const struct ai_avatar_catalog *catalog, bool motion,
                       char label[ASTER_AI_AVATAR_LABEL_CAP])
{
    if (!value)
        return true;
    const char *word = cJSON_GetStringValue(value);
    if (!word || !word[0])
        return false;
    for (unsigned i = 0; i < catalog->count; i++) {
        const struct ai_avatar_clip *clip = &catalog->clips[i];
        if ((clip->frames > 1) == motion && !strcmp(word, clip->label)) {
            memcpy(label, clip->label, strlen(clip->label) + 1);
            return true;
        }
    }
    return false;
}

static bool read_joints(const cJSON *args, uint64_t supported, struct ai_avatar_cue *next)
{
    const cJSON *joints = cJSON_GetObjectItemCaseSensitive(args, "joints");
    const cJSON *keep = cJSON_GetObjectItemCaseSensitive(args, "keep_pose");
    if ((joints || keep) && !supported)
        return false;
    if (keep && !cJSON_IsBool(keep))
        return false;
    next->keep_pose = cJSON_IsTrue(keep);
    if (!joints)
        return true;
    if (!cJSON_IsObject(joints))
        return false;
    const cJSON *value;
    cJSON_ArrayForEach(value, joints) {
        unsigned i;
        for (i = 0; i < AI_JOINT_COUNT && strcmp(value->string, joint_names[i]); i++) {}
        if (i == AI_JOINT_COUNT || !(supported & (UINT64_C(1) << i)) ||
            (next->joint_mask & (UINT64_C(1) << i)) || !cJSON_IsNumber(value) ||
            !isfinite(value->valuedouble) || value->valuedouble < -1 || value->valuedouble > 1)
            return false;
        next->joint_mask |= UINT64_C(1) << i;
        next->joints[i] = (int8_t)(100 * value->valuedouble);
    }
    return next->joint_mask != 0;
}

bool ai_avatar_tool_resolve(const struct ai_avatar_call *call, const struct ai_avatar_catalog *catalog,
                            struct ai_avatar_cue *cue, char *speech, size_t capacity)
{
    if (!call->seen || !call->named || !call->finished || call->invalid)
        return false;
    cJSON *args = cJSON_ParseWithOpts(call->arguments, NULL, true);
    struct ai_avatar_cue next = {0};
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "speech"));
    bool ok = cJSON_IsObject(args) && text && text[strspn(text, " \t\r\n")] && strlen(text) < capacity &&
              read_label(cJSON_GetObjectItemCaseSensitive(args, "motion"), catalog, true, next.motion) &&
              read_label(cJSON_GetObjectItemCaseSensitive(args, "expression"), catalog, false, next.expression) &&
              read_joints(args, catalog->joint_mask, &next) && (next.motion[0] || next.expression[0] || next.joint_mask);
    if (ok) {
        *cue = next;
        if (speech)
            memcpy(speech, text, strlen(text) + 1);
    }
    cJSON_Delete(args);
    return ok;
}
