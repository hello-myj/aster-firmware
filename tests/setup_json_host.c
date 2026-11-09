/* SPDX-License-Identifier: MIT */
#include "aster_setup_json.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    aster_setup_json j;
    assert(!aster_setup_json_parse(
        &j, " {\"ssid\":\"中文\\\"Wi-Fi\",\"bytes\":18446744073709551615} \n"));
    assert(!strcmp(aster_setup_json_get(&j, "ssid")->text, "中文\"Wi-Fi"));
    assert(aster_setup_json_get(&j, "bytes")->number == UINT64_MAX);
    const char *invalid[] = {"{\"x\":1,\"x\":2}",
                             "{\"x\":00}",
                             "{\"x\":01}",
                             "{\"x\":-1}",
                             "{\"x\":1.0}",
                             "{\"x\":18446744073709551616}",
                             "{\"x\":{}}",
                             "{\"x\":[1]}",
                             "{\"x\":\"\\u0000\"}",
                             "{\"x\":\"a\n\"}",
                             "{}garbage",
                             "{\"x\":1,}",
                             "{\"x\":"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++)
        assert(aster_setup_json_parse(&j, invalid[i]));
    char long_value[400];
    memset(long_value, 'a', sizeof(long_value));
    memcpy(long_value, "{\"x\":\"", 6);
    strcpy(long_value + 350, "\"}");
    assert(aster_setup_json_parse(&j, long_value));
    char escaped[80];
    assert(aster_setup_json_quote(escaped, sizeof(escaped), "中文\"\\") > 0);
    char object[100];
    snprintf(object, sizeof(object), "{\"ssid\":%s}", escaped);
    assert(!aster_setup_json_parse(&j, object));
    assert(!strcmp(aster_setup_json_get(&j, "ssid")->text, "中文\"\\"));
    assert(aster_setup_json_quote(escaped, 4, "long") < 0);
    puts("PASS setup strict JSON, UTF-8 and bounds");
    return 0;
}
