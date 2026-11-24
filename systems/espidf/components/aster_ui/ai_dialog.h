/* SPDX-License-Identifier: MIT */
#pragma once
#include "deepseek.h"

enum dialog_phase {
    DIALOG_IDLE, DIALOG_STARTING, DIALOG_RECORDING, DIALOG_FOLLOWUP, DIALOG_VOICED, DIALOG_NETWORK, DIALOG_ASR, DIALOG_THINKING,
    DIALOG_SYNTH, DIALOG_SPEAKING, DIALOG_CANCELLING, DIALOG_DONE, DIALOG_FAILED
};

struct dialog_view {
    char question[AI_QUESTION_CAP], answer[AI_REPLY_CAP];
    enum dialog_phase failed_phase;
    int error, http;
};

enum dialog_phase ai_dialog_phase(void);
const char *ai_dialog_name(enum dialog_phase phase);
void ai_dialog_snapshot(struct dialog_view *view);
unsigned ai_dialog_revision(void);
enum ai_query_source { AI_MICROPHONE, AI_BENCH_CLAUSE, AI_BENCH_WHOLE, AI_BENCH_WARM };
void ai_dialog_start(enum ai_query_source source);
void ai_dialog_submit(void);
void ai_dialog_reset(void);
