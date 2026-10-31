/* SPDX-License-Identifier: MIT */
#pragma once
#include <aster/wifi.h>
#include <aster/error.h>
#include "esp_netif.h"
#include "esp_event.h"
int aster_idf_wifi_service_start(esp_netif_t *station);
void aster_idf_wifi_event(esp_event_base_t base,int32_t id,void *data);
int aster_idf_wifi_submit(uint32_t operation,const char *ssid,const char *password);
int aster_idf_wifi_snapshot(aster_wifi_info_t *out);
int aster_idf_wifi_results(aster_wifi_ap_info_t *out,uint32_t capacity);
int aster_idf_wifi_saved(char out[33]);
bool aster_idf_wifi_available(void);
int aster_idf_wifi_hotspot_submit(bool enable,const char *ssid,const char *password);
void aster_idf_wifi_hotspot_snapshot(char ssid[33],bool *enabled,bool *configured);
