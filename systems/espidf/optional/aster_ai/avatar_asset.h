/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define ASTER_AI_AVATAR_PATH "/SD:/aster/ai-avatar.bin"
#define ASTER_AI_AVATAR_UPPER_PATH "/SD:/aster/ai-avatar-upper.bin"
#define ASTER_AI_AVATAR_UPPER_HD_PATH "/SD:/aster/ai-avatar-upper-hd.bin"
#define ASTER_AI_AVATAR_CLOSE_HD_PATH "/SD:/aster/ai-avatar-close-hd.bin"
#define ASTER_AI_AVATAR_MAX_CLIPS 64
#define ASTER_AI_AVATAR_LABEL_CAP 48

/* The logical channels use the same names as the desktop rig. */
#define ASTER_RIG_JOINTS(X) \
    X(head_yaw, ParamAngleX) X(head_pitch, ParamAngleY) X(head_roll, ParamAngleZ) \
    X(left_shoulder, LArm) X(left_elbow, LForearm) X(left_wrist, LWrist) X(left_fingers, LHand) \
    X(right_shoulder, RArm) X(right_elbow, RForearm) X(right_wrist, RWrist) X(right_fingers, RHand) \
    X(left_finger_detail, LHand2) X(right_finger_detail, RHand2) \
    X(raised_left_shoulder, Param36) X(raised_left_shoulder_depth, Param39) X(raised_left_elbow, Param41) \
    X(raised_left_wrist, Param42) X(raised_left_index, Param43) X(chest_right_shoulder, Param62) \
    X(chest_right_shoulder_depth, Param59) X(chest_right_elbow, Param60) X(chest_right_wrist, Param61) \
    X(left_eye, ParamEyeLOpen) X(right_eye, ParamEyeROpen) X(jaw, ParamMouthOpenY) X(mouth_form, ParamMouthForm) \
    X(jaw_hinge, JawOpen) X(gaze_x, ParamEyeBallX) X(gaze_y, ParamEyeBallY) \
    X(body_yaw, ParamBodyAngleX) X(body_pitch, ParamBodyAngleY) X(body_roll, ParamBodyAngleZ) \
    X(body_shift, ParamBodyAngleRX) X(shoulder_height, ShoulderY) X(shoulder_roll, ShoulderZ) \
    X(hip_yaw, HipAngleX) X(legs_sway, LEGAngleZ2) X(legs_spread, Param33) \
    X(tail_flip, Param52) X(tail_1, Param_Angle_Rotation84) X(tail_2, Param_Angle_Rotation85) \
    X(tail_3, Param_Angle_Rotation86) X(tail_4, Param_Angle_Rotation87) X(tail_5, Param_Angle_Rotation88) \
    X(tail_6, Param_Angle_Rotation89)

#define ASTER_RIG_JOINT_INDEX(name, id) AI_JOINT_##name,
enum ai_rig_joint { ASTER_RIG_JOINTS(ASTER_RIG_JOINT_INDEX) AI_JOINT_COUNT };
#undef ASTER_RIG_JOINT_INDEX

/* Little-endian AWA1 clip record. The player can expose these validated
 * labels to the model without loading or decoding any additional frames. */
struct ai_avatar_clip {
    uint32_t first;
    uint16_t frames, loop;
    char label[ASTER_AI_AVATAR_LABEL_CAP];
};

struct ai_avatar_catalog {
    const struct ai_avatar_clip *clips;
    unsigned count;
    uint64_t joint_mask;
    bool explicit_only;
};

struct ai_avatar_cue {
    char motion[ASTER_AI_AVATAR_LABEL_CAP];
    char expression[ASTER_AI_AVATAR_LABEL_CAP];
    uint64_t joint_mask;
    int8_t joints[AI_JOINT_COUNT];
    bool keep_pose;
};
