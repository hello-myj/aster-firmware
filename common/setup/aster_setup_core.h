/* SPDX-License-Identifier: MIT */
#ifndef ASTER_SETUP_CORE_H
#define ASTER_SETUP_CORE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef enum {
    ASTER_SETUP_CHECK,
    ASTER_SETUP_STORAGE,
    ASTER_SETUP_WELCOME,
    ASTER_SETUP_PROVISION,
    ASTER_SETUP_CONNECT,
    ASTER_SETUP_PLAN,
    ASTER_SETUP_DOWNLOAD,
    ASTER_SETUP_VERIFY,
    ASTER_SETUP_INSTALL,
    ASTER_SETUP_DONE,
    ASTER_SETUP_ERROR,
    ASTER_SETUP_FORMAT
} aster_setup_phase_t;
typedef struct {
    aster_setup_phase_t phase, failed_phase;
    uint64_t bytes, total;
    uint32_t file, files;
    int error;
    bool required;
} aster_setup_state_t;
typedef struct {
    char ssid[33], password[65], server[192];
} aster_setup_credentials_t;
/* Called on the setup worker. UI/network callbacks enqueue commands instead
 * of mutating this state or performing filesystem/network operations. */
void aster_setup_core_init(aster_setup_state_t *s, bool storage, bool complete, bool configured);
void aster_setup_core_step(aster_setup_state_t *s, aster_setup_phase_t next);
void aster_setup_core_fail(aster_setup_state_t *s, int error);
void aster_setup_core_retry(aster_setup_state_t *s, bool connected);
unsigned aster_setup_core_percent(const aster_setup_state_t *s);
bool aster_setup_server_url(const char *url);
bool aster_setup_credentials_valid(const aster_setup_credentials_t *c);
const char *aster_setup_phase_title(aster_setup_phase_t phase);
#endif
