/* SPDX-License-Identifier: MIT */
#include "aster_vad.h"
#include "esp_vadn_models.h"
#include "model_path.h"
#include <errno.h>

extern const uint8_t voice_models[] asm("_binary_aster_voice_models_bin_start");
static srmodel_list_t *models;
static const esp_vadn_iface_t *engine;
static model_iface_data_t *session;

void aster_vad_close(void)
{
    if (session)
        engine->destroy(session);
    session = NULL;
    if (models)
        srmodel_host_deinit(models);
    models = NULL;
    engine = NULL;
}

int aster_vad_open(unsigned silence_ms)
{
    models = srmodel_load(voice_models);
    const char *name = models ? esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL) : NULL;
    engine = name ? esp_vadn_handle_from_name(name) : NULL;
    if (engine)
        session = engine->create(name, VAD_MODE_1, 1, 128, silence_ms);
    int frame = session ? engine->get_samp_chunksize(session) : -ENODEV;
    if (session && (engine->get_samp_rate(session) != 16000 ||
                    engine->get_channel_num(session) != 1 || frame <= 0 || frame > 2048))
        frame = -ENOTSUP;
    if (frame < 0)
        aster_vad_close();
    return frame;
}

int aster_vad_detect(int16_t *frame)
{
    vad_state_t state = engine->detect(session, frame);
    return state == VAD_SPEECH ? 1 : state == VAD_SILENCE ? 0 : -EIO;
}
