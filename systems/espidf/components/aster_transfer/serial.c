#include "aster_power.h"
#ifdef ASTER_AI_ENABLED
#include "ai_credentials.h"
#endif
/* SPDX-License-Identifier: MIT */
#include "transfer.h"
#include "aster_storage.h"
#include "aster_sysinfo.h"
#include "aster_deploy.h"
#include <aster/system.h>
#include "esp_console.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static int ping(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint64_t caps = 0;
    for (unsigned bit = 0; bit < 64; bit++)
        if (aster_sysinfo_has_feature(bit))
            caps |= 1ULL << bit;
    printf(
        "\nPONG sd=%d product=aster target=riscv abi=%u.%u caps=%llx serial=shared transfer=packages board=%s\n",
        aster_storage_mounted() ? 1 : 0, ASTER_ABI_MAJOR, ASTER_ABI_MINOR,
        (unsigned long long)caps, CONFIG_ASTER_BOARD_ID);
    fflush(stdout);
    return 0;
}

static int receive(void *context, uint8_t *data, size_t size)
{
    (void)context;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    int n = usb_serial_jtag_read_bytes(data, size, pdMS_TO_TICKS(30000));
#else
    int n = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, data, size, pdMS_TO_TICKS(30000));
#endif
    return n > 0 ? n : -ETIMEDOUT;
}

static void reply(void *context, const char *line)
{
    (void)context;
    printf("\n%s\n", line);
    fflush(stdout);
}

static void progress(void *context, unsigned phase, uint32_t done, uint32_t total, int error)
{
    (void)context;
    if (error) {
        aster_deploy_finish(error);
        return;
    }
    static const aster_deploy_phase phases[] = {ASTER_DEPLOY_RECEIVE, ASTER_DEPLOY_VERIFY,
                                                ASTER_DEPLOY_RECEIVED};
    if (phase < 3)
        aster_deploy_update(phases[phase], done, total, NULL);
}

static int put(int argc, char **argv)
{
    /* The SDK sender waits for ACK before sending a byte. While this handler
     * runs, REPL isn't reading UART; read the driver directly so CR/LF and
     * binary NUL never pass through stdio's terminal transformations. */
    if (argc != 4 && argc != 5) {
        reply(NULL, "ERR -22");
        return 0;
    }
    char path[192];
    if (aster_transfer_package_path(argv[1], path, sizeof(path))) {
        reply(NULL, "ERR -1");
        return 0;
    }
    if (!aster_storage_mounted()) {
        reply(NULL, "ERR -19");
        return 0;
    }
    uint32_t values[3] = {0};
    for (int i = 2; i < argc; i++) {
        char *end;
        errno = 0;
        unsigned long value = strtoul(argv[i], &end, i == 3 ? 16 : 10);
        if (!argv[i][0] || argv[i][0] == '-' || *end || errno || value > UINT32_MAX) {
            reply(NULL, "ERR -22");
            return 0;
        }
        values[i - 2] = (uint32_t)value;
    }
    if (aster_power_activity_begin()) {
        reply(NULL, "ERR -16");
        return 0;
    }
    aster_deploy_begin(argv[1], values[0], values[2]);
    const aster_transfer_io_t io = {receive, reply, NULL, progress};
    int result = aster_transfer_receive(path, values[0], values[1], values[2], &io);
    aster_power_activity_end();
    if (result) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
        uint8_t discarded[64];
        while (usb_serial_jtag_read_bytes(discarded, sizeof(discarded), 0) > 0) {
        }
#else
        uart_flush_input(CONFIG_ESP_CONSOLE_UART_NUM);
#endif
    }
    return 0;
}

#ifdef ASTER_AI_ENABLED
static int read_key_payload(void *key, size_t length)
{
    reply(NULL, "AIKEY READY");
    for (size_t received = 0; received < length;) {
        int n = receive(NULL, (uint8_t *)key + received, length - received);
        if (n <= 0)
            return -ETIMEDOUT;
        received += n;
    }
    return 0;
}

static int provision_key(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "speech-clear")) {
        reply(NULL, "AIKEY READY");
        reply(NULL, ai_speech_keys_clear() ? "AIKEY ERROR" : "AIKEY OK");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "speech")) {
        struct ai_speech_keys keys;
        int rc = read_key_payload(&keys, sizeof(keys));
        if (!rc)
            rc = ai_speech_keys_write(&keys);
        ai_secret_clear(&keys, sizeof(keys));
        reply(NULL, rc ? "AIKEY ERROR" : "AIKEY OK");
        return 0;
    }
    char key[AI_KEY_CAP] = {0}, *end = NULL;
    long length = argc == 2 ? strtol(argv[1], &end, 10) : -1;
    if (length < 0 || length >= sizeof(key) || !end || *end || !argv[1][0] ||
        (length && length < 12)) {
        reply(NULL, "AIKEY ERROR");
        return 0;
    }
    int rc = read_key_payload(key, length);
    if (!rc)
        rc = ai_key_write(key, length);
    ai_secret_clear(key, sizeof(key));
    reply(NULL, rc ? "AIKEY ERROR" : "AIKEY OK");
    return 0;
}
#endif

int aster_idf_transfer_register(void)
{
    const esp_console_cmd_t probe = {
        .command = "ASTERPING", .help = "Probe ASTERFS shared UART", .func = ping};
    const esp_console_cmd_t upload = {
        .command = "ASTERPUT", .help = "Receive an ASTP package using ASTERFS 3", .func = put};
    esp_err_t result = esp_console_cmd_register(&probe);
    if (result == ESP_OK)
        result = esp_console_cmd_register(&upload);
#ifdef ASTER_AI_ENABLED
    const esp_console_cmd_t provision = {
        .command = "ASTERAIKEY", .help = "Provision voice service credential over USB", .func = provision_key};
    if (result == ESP_OK)
        result = esp_console_cmd_register(&provision);
#endif
    return result;
}
