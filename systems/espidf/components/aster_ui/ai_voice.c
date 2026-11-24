/* SPDX-License-Identifier: MIT */
#include "ai_voice.h"
#include "aster_device.h"
#include "aster_wake.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>

static atomic_int status = AI_IDLE;
static atomic_bool stopping;

static void listen(void *arg)
{
    (void)arg;
    int samples = aster_wake_open();
    int rc = samples <= 0 ? (samples ? samples : -EINVAL) : 0;
    int mic = -1;
    aster_audio_info_t info = {0};
    int16_t *frame = NULL;
    bool heard = false;
    if (!rc && !atomic_load(&stopping)) {
        mic = aster_device_audio_capture_open(&info);
        if (mic < 0)
            rc = mic;
        else if (info.sample_rate != 16000 || info.bits != 16 ||
                 (info.channels != 1 && info.channels != 2) || samples > 4096)
            rc = -ENOTSUP;
        else if (!(frame = malloc(samples * info.channels * sizeof(*frame))))
            rc = -ENOMEM;
    }
    if (!rc && mic >= 0) {
        size_t have = 0, bytes = samples * info.channels * sizeof(*frame);
        atomic_store(&status, AI_LISTENING);
        ESP_LOGI("aster_ai", "offline listening: %lu Hz, %lu ch, %d samples/frame",
                 (unsigned long)info.sample_rate, (unsigned long)info.channels, samples);
        while (!atomic_load(&stopping)) {
            int n = aster_device_audio_capture_read(mic, (uint8_t *)frame + have, bytes - have);
            if (n == -EAGAIN || n == 0) {
                vTaskDelay(1);
                continue;
            }
            if (n < 0) {
                rc = n;
                break;
            }
            have += n;
            if (have != bytes)
                continue;
            if (info.channels == 2) {
                /* Consume the left microphone slot of the native I2S frame. */
                for (int i = 0; i < samples; ++i)
                    frame[i] = frame[2 * i];
            }
            int word = aster_wake_detect(frame);
            if (word > 0) {
                heard = true;
                break;
            }
            have = 0;
        }
    }
    if (mic >= 0)
        aster_device_audio_capture_close(mic);
    free(frame);
    aster_wake_close();
    if (heard)
        ESP_LOGI("aster_ai", "wake word detected");
    else if (rc)
        ESP_LOGW("aster_ai", "listen ended: %d", rc);
    enum ai_voice_status end = heard ? AI_HEARD : rc == -EBUSY ? AI_BUSY : rc ? AI_FAILED : AI_IDLE;
    atomic_store(&status, atomic_load(&stopping) ? AI_IDLE : end);
    vTaskDelete(NULL);
}

enum ai_voice_status ai_voice_status(void)
{
    return atomic_load(&status);
}

/* Both control methods run on the single Launcher/UI worker. */
void ai_voice_start(void)
{
    if (ai_voice_status() != AI_IDLE)
        return;
    atomic_store(&stopping, false);
    atomic_store(&status, AI_LOADING);
    if (xTaskCreate(listen, "ai_wake", 12288, NULL, 4, NULL) != pdPASS)
        atomic_store(&status, AI_FAILED);
}

void ai_voice_stop(void)
{
    atomic_store(&stopping, true);
    for (;;) {
        enum ai_voice_status current = ai_voice_status();
        if (current != AI_LISTENING && current != AI_LOADING)
            break;
        vTaskDelay(1);
    }
    atomic_store(&status, AI_IDLE);
}
