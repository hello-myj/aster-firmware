/* SPDX-License-Identifier: MIT */
#include "aster_wake.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include <errno.h>

extern const uint8_t voice_models[] asm("_binary_aster_voice_models_bin_start");
static srmodel_list_t *models;
static const esp_wn_iface_t *engine;
static model_iface_data_t *session;

int aster_wake_open(void)
{
    models = srmodel_load(voice_models);
    char *name = models ? esp_srmodel_filter(models, ESP_WN_PREFIX, NULL) : NULL;
    if (name) {
        engine = esp_wn_handle_from_name(name);
        if (engine)
            session = engine->create(name, DET_MODE_90);
    }
    if (!session)
        return -ENODEV;
    if (engine->get_samp_rate(session) != 16000 || engine->get_channel_num(session) != 1)
        return -ENOTSUP;
    return engine->get_samp_chunksize(session);
}

int aster_wake_detect(int16_t *frame)
{
    return engine->detect(session, frame);
}

void aster_wake_close(void)
{
    if (session)
        engine->destroy(session);
    session = NULL;
    if (models)
        srmodel_host_deinit(models);
    models = NULL;
}
