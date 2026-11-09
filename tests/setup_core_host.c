#include <assert.h>
#include <string.h>
#include "aster_setup_core.h"
int main(void) {
    aster_setup_state_t s;
    aster_setup_core_init(&s, false, false, false);
    assert(s.required && s.phase == ASTER_SETUP_STORAGE);
    aster_setup_core_init(&s, true, true, false);
    assert(!s.required && s.phase == ASTER_SETUP_DONE);
    aster_setup_core_init(&s, true, false, false);
    assert(s.required && s.phase == ASTER_SETUP_WELCOME);
    aster_setup_core_init(&s, true, false, true);
    assert(s.required && s.phase == ASTER_SETUP_CONNECT);
    aster_setup_core_step(&s, ASTER_SETUP_DOWNLOAD);
    s.bytes = 50;
    s.total = 100;
    assert(aster_setup_core_percent(&s) == 50);
    s.bytes = 200;
    assert(aster_setup_core_percent(&s) == 100);
    aster_setup_core_fail(&s, -5);
    assert(s.required && s.phase == ASTER_SETUP_ERROR && s.failed_phase == ASTER_SETUP_DOWNLOAD);
    aster_setup_core_retry(&s, true);
    assert(s.phase == ASTER_SETUP_PLAN && !s.error && !s.bytes && !s.total);
    aster_setup_core_step(&s, ASTER_SETUP_DONE);
    assert(!s.required && aster_setup_core_percent(&s) == 100);
    aster_setup_credentials_t c = {
        .ssid = "测试网络", .password = "test-pass", .server = "http://192.168.110.154:9081"};
    assert(aster_setup_credentials_valid(&c));
    strcpy(c.password, "short");
    assert(!aster_setup_credentials_valid(&c));
    c.password[0] = 0;
    assert(aster_setup_credentials_valid(&c));
    strcpy(c.server, "http://host/\r\nX: 1");
    assert(!aster_setup_credentials_valid(&c));
    assert(!aster_setup_server_url("http://user:password@host"));
    assert(!aster_setup_server_url("https:///missing"));
    assert(aster_setup_server_url("https://resources.example.com/aster"));
    return 0;
}
