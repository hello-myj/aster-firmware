/* SPDX-License-Identifier: MIT */
#include "aster_setup_core.h"
#include <string.h>
void aster_setup_core_init(aster_setup_state_t *s, bool storage, bool complete, bool configured) {
    memset(s, 0, sizeof(*s));
    s->required = !storage || !complete;
    s->phase = !storage     ? ASTER_SETUP_STORAGE
               : complete   ? ASTER_SETUP_DONE
               : configured ? ASTER_SETUP_CONNECT
                            : ASTER_SETUP_WELCOME;
}
void aster_setup_core_step(aster_setup_state_t *s, aster_setup_phase_t next) {
    s->phase = next;
    s->error = 0;
    if (next == ASTER_SETUP_DONE)
        s->required = false;
}
void aster_setup_core_fail(aster_setup_state_t *s, int error) {
    s->failed_phase = s->phase;
    s->phase = ASTER_SETUP_ERROR;
    s->error = error;
    s->required = true;
}
void aster_setup_core_retry(aster_setup_state_t *s, bool connected) {
    s->phase = connected ? ASTER_SETUP_PLAN : ASTER_SETUP_PROVISION;
    s->error = 0;
    s->bytes = s->total = 0;
    s->file = s->files = 0;
}
unsigned aster_setup_core_percent(const aster_setup_state_t *s) {
    if (s->phase == ASTER_SETUP_DONE)
        return 100;
    if (!s->total)
        return 0;
    uint64_t bytes = s->bytes > s->total ? s->total : s->bytes;
    /* Avoid overflow for malicious metadata; a setup release is <=1GiB. */
    return bytes > UINT64_MAX / 100 ? 0 : (unsigned)(bytes * 100 / s->total);
}
bool aster_setup_server_url(const char *url) {
    if (!url)
        return false;
    size_t n = strlen(url);
    if (n < 8 || n >= 192)
        return false;
    const char *host = !strncmp(url, "http://", 7)    ? url + 7
                       : !strncmp(url, "https://", 8) ? url + 8
                                                      : NULL;
    if (!host || !*host || *host == '/' || *host == ':')
        return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p <= 32 || *p >= 127 || *p == '@' || *p == '?' || *p == '#' || *p == '\\')
            return false;
    return true;
}
bool aster_setup_credentials_valid(const aster_setup_credentials_t *c) {
    if (!c || !memchr(c->ssid, 0, sizeof(c->ssid)) ||
        !memchr(c->password, 0, sizeof(c->password)) || !memchr(c->server, 0, sizeof(c->server)))
        return false;
    if (!c->ssid[0] || !aster_setup_server_url(c->server))
        return false;
    size_t n = strlen(c->password);
    if (n && n < 8)
        return false;
    if (n == 64)
        for (unsigned i = 0; i < 64; i++)
            if (!((c->password[i] >= '0' && c->password[i] <= '9') ||
                  (c->password[i] >= 'a' && c->password[i] <= 'f') ||
                  (c->password[i] >= 'A' && c->password[i] <= 'F')))
                return false;
    return true;
}
const char *aster_setup_phase_title(aster_setup_phase_t phase) {
    static const char *const titles[] = {"检查系统",     "检查存储卡",   "欢迎使用 Aster",
                                         "使用手机连接", "正在连接网络", "检查系统资源",
                                         "准备系统资源", "正在校验",     "正在安装商城",
                                         "准备完成",     "需要你的帮助","正在格式化存储卡"};
    return (unsigned)phase < sizeof(titles) / sizeof(*titles) ? titles[phase]
                                                              : titles[ASTER_SETUP_ERROR];
}
