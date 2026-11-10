/* SPDX-License-Identifier: MIT */
#ifndef ASTER_SETUP_H
#define ASTER_SETUP_H
#include "aster_setup_core.h"
#define ASTER_SETUP_DEFAULT_SOURCE "https://github.com/hello-myj/aster-releases/releases/latest/download/aster-source.json"
typedef struct {
    aster_setup_state_t state;
    char ssid[33], password[17], token[33], server[192], file[64];
    bool hotspot;
    uint32_t seconds;
} aster_setup_snapshot;
/* UI owner only. Starts lazily, reaps temporary worker stacks at completion.
 * Returns true while first-boot UI owns the screen. */
bool aster_setup_poll(aster_setup_snapshot *out);
void aster_setup_get(aster_setup_snapshot *out);
void aster_setup_begin(void);
void aster_setup_retry(void);
/* Maintenance only: reopen onboarding without deleting installed resources. */
int aster_setup_repair(void);
/* Maintenance: verify/download from a new source using the saved Wi-Fi. */
int aster_setup_repair_source(const char *url);
/* Trusted CLI only; the caller must require an explicit confirmation token. */
int aster_setup_request_format(void);
bool aster_setup_format_pending(void);
void aster_setup_format_ui_ready(void);
int aster_setup_submit(const aster_setup_credentials_t *credentials);
#endif
