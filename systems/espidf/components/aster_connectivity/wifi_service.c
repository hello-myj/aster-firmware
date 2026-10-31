/* SPDX-License-Identifier: MIT */
#include "wifi_service.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>
typedef struct {uint32_t operation;char ssid[33],password[65];} request_t;
typedef struct {char ssid[33],password[65];} profile_t;
static SemaphoreHandle_t guard;
static QueueHandle_t queue;
static EventGroupHandle_t events;
static esp_netif_t *station;
static aster_wifi_info_t info;
static aster_wifi_ap_info_t results[ASTER_WIFI_AP_MAX];
static unsigned result_count;
static bool ready,scan_done;
static profile_t saved;
static profile_t hotspot;
static bool hotspot_enabled;
#define GOT_IP BIT0
#define AP_UP BIT1
#define HOTSPOT_START 6
#define HOTSPOT_STOP 7
static int security(wifi_auth_mode_t mode)
{
    if(mode==WIFI_AUTH_OPEN)return ASTER_WIFI_SECURITY_OPEN;
    if(mode==WIFI_AUTH_WEP)return ASTER_WIFI_SECURITY_WEP;
    if(mode==WIFI_AUTH_WPA2_ENTERPRISE)return ASTER_WIFI_SECURITY_ENTERPRISE;
    return ASTER_WIFI_SECURITY_PERSONAL;
}
static int error(esp_err_t r)
{
    if(r==ESP_OK)return 0;
    if(r==ESP_ERR_NO_MEM)return ASTER_ENOMEM;
    if(r==ESP_ERR_INVALID_ARG)return ASTER_EINVAL;
    if(r==ESP_ERR_TIMEOUT)return ASTER_ETIMEDOUT;
    return ASTER_EIO;
}
bool aster_idf_wifi_available(void)
{if(!guard)return false;xSemaphoreTake(guard,portMAX_DELAY);bool v=ready;xSemaphoreGive(guard);return v;}
int aster_idf_wifi_snapshot(aster_wifi_info_t *out)
{
    if(!guard || !out)return ASTER_ESTATE;
    xSemaphoreTake(guard,portMAX_DELAY);*out=info;xSemaphoreGive(guard);return 0;
}
int aster_idf_wifi_saved(char out[33])
{
    if(!guard)return ASTER_ESTATE;
    xSemaphoreTake(guard,portMAX_DELAY);memcpy(out,saved.ssid,33);xSemaphoreGive(guard);
    return out[0]?0:ASTER_ENOENT;
}
int aster_idf_wifi_results(aster_wifi_ap_info_t *out,uint32_t capacity)
{
    if(!guard)return ASTER_ESTATE;
    xSemaphoreTake(guard,portMAX_DELAY);
    int n=info.operation==ASTER_WIFI_OP_SCAN?ASTER_EAGAIN:!scan_done?ASTER_ENOENT:(int)result_count;
    if(n>=0){if((unsigned)n>capacity)n=capacity;if(n)memcpy(out,results,n*sizeof(*out));}
    xSemaphoreGive(guard);return n;
}
int aster_idf_wifi_submit(uint32_t op,const char *ssid,const char *password)
{
    if(op<ASTER_WIFI_OP_SCAN || op>ASTER_WIFI_OP_FORGET)return ASTER_EINVAL;
    request_t request={.operation=op};
    if(op==ASTER_WIFI_OP_CONNECT) {
        if(!ssid || !*ssid || strlen(ssid)>32 || !password || strlen(password)>64)return ASTER_EINVAL;
        strcpy(request.ssid,ssid);strcpy(request.password,password);
    }
    if(!guard)return ASTER_ESTATE;
    xSemaphoreTake(guard,portMAX_DELAY);
    int r=0;
    if(!ready)r=ASTER_ESTATE;
    else if(info.operation)r=ASTER_EBUSY;
    else if(op==ASTER_WIFI_OP_RECONNECT && !saved.ssid[0])r=ASTER_ENOENT;
    else {
        if(op==ASTER_WIFI_OP_RECONNECT){strcpy(request.ssid,saved.ssid);strcpy(request.password,saved.password);}
        info.operation=op;
        if(xQueueSend(queue,&request,0)!=pdTRUE){info.operation=0;r=ASTER_EBUSY;}
    }
    xSemaphoreGive(guard);memset(&request,0,sizeof(request));return r;
}
void aster_idf_wifi_hotspot_snapshot(char ssid[33],bool *enabled,bool *configured)
{
    if(!guard){ssid[0]=0;*enabled=*configured=false;return;}
    xSemaphoreTake(guard,portMAX_DELAY);memcpy(ssid,hotspot.ssid,33);
    *enabled=hotspot_enabled;*configured=hotspot.ssid[0]!=0;xSemaphoreGive(guard);
}
int aster_idf_wifi_hotspot_submit(bool enable,const char *ssid,const char *password)
{
    if(!guard)return ASTER_ESTATE;
    request_t request={.operation=enable?HOTSPOT_START:HOTSPOT_STOP};
    xSemaphoreTake(guard,portMAX_DELAY);int r=0;
    if(!ready)r=ASTER_ESTATE;
    else if(info.operation)r=ASTER_EBUSY;
    else {
        if(enable) {
            const char *name=ssid?ssid:hotspot.ssid;
            const char *pass=password && *password?password:hotspot.password;
            if(!*name || strlen(name)>32 || strlen(pass)<8 || strlen(pass)>63)r=ASTER_EINVAL;
            else {strcpy(request.ssid,name);strcpy(request.password,pass);}
        }
        if(!r){info.operation=request.operation;if(xQueueSend(queue,&request,0)!=pdTRUE){info.operation=0;r=ASTER_EBUSY;}}
    }
    xSemaphoreGive(guard);memset(&request,0,sizeof(request));return r;
}
void aster_idf_wifi_event(esp_event_base_t base,int32_t id,void *data)
{
    if(!guard)return;
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        wifi_ap_record_t ap={0};esp_err_t r=esp_wifi_sta_get_ap_info(&ap);
        ip_event_got_ip_t *ip=data;
        xSemaphoreTake(guard,portMAX_DELAY);
        info.state=ASTER_WIFI_STATE_COMPLETED;
        snprintf(info.ipv4,sizeof(info.ipv4),IPSTR,IP2STR(&ip->ip_info.ip));
        if(r==ESP_OK){memcpy(info.ssid,ap.ssid,32);info.ssid[32]=0;info.rssi=ap.rssi;info.security=security(ap.authmode);}
        xSemaphoreGive(guard);xEventGroupSetBits(events,GOT_IP);
    } else if(base==WIFI_EVENT && (id==WIFI_EVENT_STA_DISCONNECTED || id==WIFI_EVENT_STA_STOP)) {
        xSemaphoreTake(guard,portMAX_DELAY);
        info.state=ASTER_WIFI_STATE_DISCONNECTED;info.ipv4[0]=0;info.rssi=0;
        xSemaphoreGive(guard);xEventGroupClearBits(events,GOT_IP);
    } else if(base==WIFI_EVENT && id==WIFI_EVENT_STA_CONNECTED) {
        xSemaphoreTake(guard,portMAX_DELAY);info.state=ASTER_WIFI_STATE_ASSOCIATED;xSemaphoreGive(guard);
    } else if(base==WIFI_EVENT && (id==WIFI_EVENT_AP_START || id==WIFI_EVENT_AP_STOP)) {
        xSemaphoreTake(guard,portMAX_DELAY);hotspot_enabled=id==WIFI_EVENT_AP_START;xSemaphoreGive(guard);
        if(id==WIFI_EVENT_AP_START)xEventGroupSetBits(events,AP_UP);
        else xEventGroupClearBits(events,AP_UP);
    }
}
static int store_named(const char *key,const profile_t *profile)
{
    nvs_handle_t handle;esp_err_t r=nvs_open("asterwifi",NVS_READWRITE,&handle);
    if(r!=ESP_OK)return error(r);
    r=profile?nvs_set_blob(handle,key,profile,sizeof(*profile)):nvs_erase_key(handle,key);
    if(r==ESP_ERR_NVS_NOT_FOUND && !profile)r=ESP_OK;
    if(r==ESP_OK)r=nvs_commit(handle);
    nvs_close(handle);return error(r);
}
static int store(const profile_t *profile) {return store_named("profile",profile);}
static int configure_hotspot(request_t *request)
{
    wifi_ap_record_t ap;bool connected=esp_wifi_sta_get_ap_info(&ap)==ESP_OK;
    bool enable=request->operation==HOTSPOT_START;
    /* Configure while stopped: never transiently publish the default open AP. */
    esp_err_t r=esp_wifi_stop();
    if(r==ESP_OK)r=esp_wifi_set_mode(enable?WIFI_MODE_APSTA:WIFI_MODE_STA);
    if(enable && r==ESP_OK) {
        wifi_config_t config={0};memcpy(config.ap.ssid,request->ssid,strlen(request->ssid));
        memcpy(config.ap.password,request->password,strlen(request->password));
        config.ap.ssid_len=strlen(request->ssid);config.ap.channel=1;config.ap.max_connection=4;
        config.ap.authmode=WIFI_AUTH_WPA2_PSK;
        r=esp_wifi_set_config(WIFI_IF_AP,&config);memset(&config,0,sizeof(config));
    }
    if(r==ESP_OK)r=esp_wifi_start();
    if(r!=ESP_OK)return error(r);
    if(connected)esp_wifi_connect();
    if(!enable)return 0;
    if(!(xEventGroupWaitBits(events,AP_UP,pdFALSE,pdFALSE,pdMS_TO_TICKS(10000))&AP_UP))return ASTER_ETIMEDOUT;
    profile_t profile={0};strcpy(profile.ssid,request->ssid);strcpy(profile.password,request->password);
    int status=store_named("hotspot",&profile);
    if(!status){xSemaphoreTake(guard,portMAX_DELAY);hotspot=profile;xSemaphoreGive(guard);}
    memset(&profile,0,sizeof(profile));return status;
}
static int connect_ap(request_t *request)
{
    wifi_config_t config={0};
    memcpy(config.sta.ssid,request->ssid,strlen(request->ssid));
    memcpy(config.sta.password,request->password,strlen(request->password));
    /* This ABI selects 2.4 GHz station networks, including on dual-band C5. */
    config.sta.scan_method=WIFI_ALL_CHANNEL_SCAN;
    esp_wifi_disconnect();xEventGroupClearBits(events,GOT_IP);
    xSemaphoreTake(guard,portMAX_DELAY);info.state=ASTER_WIFI_STATE_ASSOCIATING;xSemaphoreGive(guard);
    esp_err_t r=esp_wifi_set_config(WIFI_IF_STA,&config);
    if(r==ESP_OK)r=esp_wifi_connect();
    memset(&config,0,sizeof(config));
    if(r!=ESP_OK)return error(r);
    if(!(xEventGroupWaitBits(events,GOT_IP,pdFALSE,pdFALSE,pdMS_TO_TICKS(30000))&GOT_IP)) {
        esp_wifi_disconnect();return ASTER_ETIMEDOUT;
    }
    profile_t profile={0};strcpy(profile.ssid,request->ssid);strcpy(profile.password,request->password);
    int status=store(&profile);
    if(!status){xSemaphoreTake(guard,portMAX_DELAY);saved=profile;xSemaphoreGive(guard);}
    memset(&profile,0,sizeof(profile));return status;
}
static int scan(void)
{
    wifi_scan_config_t config={0};
    xSemaphoreTake(guard,portMAX_DELAY);uint32_t previous=info.state;info.state=ASTER_WIFI_STATE_SCANNING;xSemaphoreGive(guard);
    esp_err_t r=esp_wifi_scan_start(&config,true);
    wifi_ap_record_t aps[ASTER_WIFI_AP_MAX];uint16_t n=ASTER_WIFI_AP_MAX;
    if(r==ESP_OK)r=esp_wifi_scan_get_ap_records(&n,aps);
    xSemaphoreTake(guard,portMAX_DELAY);
    /* Do not overwrite a link event which happened during the scan. */
    if(info.state==ASTER_WIFI_STATE_SCANNING)info.state=previous;
    if(r==ESP_OK) {
        result_count=n;scan_done=true;
        for(unsigned i=0;i<n;i++) {
            results[i]=(aster_wifi_ap_info_t){.size=sizeof(results[i]),.version=1,.rssi=aps[i].rssi,
                .channel=aps[i].primary,.security=security(aps[i].authmode)};
            memcpy(results[i].ssid,aps[i].ssid,32);
        }
        /* IDF already orders by RSSI; keep that invariant explicit. */
        for(unsigned i=1;i<n;i++)for(unsigned j=i;j && results[j].rssi>results[j-1].rssi;j--) {
            aster_wifi_ap_info_t t=results[j];results[j]=results[j-1];results[j-1]=t;
        }
    }
    xSemaphoreGive(guard);return error(r);
}
static void worker(void *unused)
{
    (void)unused;wifi_init_config_t config=WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t r=esp_wifi_init(&config);
    if(r==ESP_OK)r=esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if(r==ESP_OK)r=esp_wifi_set_mode(WIFI_MODE_STA);
    if(r==ESP_OK)r=esp_wifi_start();
    /* The remote C5 must support this API; do not guess its selected band. */
    if(r==ESP_OK)r=esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
    nvs_handle_t handle;
    profile_t loaded={0};
    if(nvs_open("asterwifi",NVS_READONLY,&handle)==ESP_OK) {
        size_t n=sizeof(loaded);
        if(nvs_get_blob(handle,"profile",&loaded,&n)!=ESP_OK || n!=sizeof(loaded) ||
           !memchr(loaded.ssid,0,sizeof(loaded.ssid)) || !memchr(loaded.password,0,sizeof(loaded.password)))memset(&loaded,0,sizeof(loaded));
        nvs_close(handle);
    }
    profile_t loaded_hotspot={0};
    if(nvs_open("asterwifi",NVS_READONLY,&handle)==ESP_OK) {
        size_t n=sizeof(loaded_hotspot);
        if(nvs_get_blob(handle,"hotspot",&loaded_hotspot,&n)!=ESP_OK || n!=sizeof(loaded_hotspot) ||
           !memchr(loaded_hotspot.ssid,0,sizeof(loaded_hotspot.ssid)) ||
           !memchr(loaded_hotspot.password,0,sizeof(loaded_hotspot.password)))memset(&loaded_hotspot,0,sizeof(loaded_hotspot));
        nvs_close(handle);
    }
    xSemaphoreTake(guard,portMAX_DELAY);saved=loaded;ready=r==ESP_OK;
    hotspot=loaded_hotspot;
    info.state=ready?ASTER_WIFI_STATE_DISCONNECTED:ASTER_WIFI_STATE_DISABLED;
    info.last_result=error(r);xSemaphoreGive(guard);
    memset(&loaded,0,sizeof(loaded));
    memset(&loaded_hotspot,0,sizeof(loaded_hotspot));
    if(r!=ESP_OK)ESP_LOGE("aster_wifi","C5 initialization failed: %s",esp_err_to_name(r));
    if(r==ESP_OK)aster_idf_wifi_submit(ASTER_WIFI_OP_RECONNECT,NULL,NULL);
    request_t request;
    for(;;) {
        xQueueReceive(queue,&request,portMAX_DELAY);int result;
        switch(request.operation) {
        case ASTER_WIFI_OP_SCAN:result=scan();break;
        case ASTER_WIFI_OP_CONNECT:case ASTER_WIFI_OP_RECONNECT:result=connect_ap(&request);break;
        case ASTER_WIFI_OP_DISCONNECT:
            r=esp_wifi_disconnect();result=r==ESP_ERR_WIFI_NOT_CONNECT?0:error(r);break;
        case ASTER_WIFI_OP_FORGET:
            esp_wifi_disconnect();result=store(NULL);
            if(!result){xSemaphoreTake(guard,portMAX_DELAY);memset(&saved,0,sizeof(saved));xSemaphoreGive(guard);}break;
        case HOTSPOT_START:case HOTSPOT_STOP:result=configure_hotspot(&request);break;
        default:result=ASTER_EINVAL;
        }
        xSemaphoreTake(guard,portMAX_DELAY);
        info.last_operation=request.operation;info.last_result=result;info.operation=0;
        xSemaphoreGive(guard);memset(&request,0,sizeof(request));
    }
}
int aster_idf_wifi_service_start(esp_netif_t *netif)
{
    if(guard)return ASTER_EBUSY;
    station=netif;(void)station;
    guard=xSemaphoreCreateMutex();events=xEventGroupCreate();queue=xQueueCreate(1,sizeof(request_t));
    info=(aster_wifi_info_t){.size=sizeof(info),.version=1,.state=ASTER_WIFI_STATE_INACTIVE};
    if(!guard || !events || !queue)goto failed;
    if(xTaskCreate(worker,"aster_wifi",8192,NULL,3,NULL)!=pdPASS)goto failed;
    return 0;
failed:
    if(queue)vQueueDelete(queue);
    if(events)vEventGroupDelete(events);
    if(guard)vSemaphoreDelete(guard);
    queue=NULL;events=NULL;guard=NULL;return ASTER_ENOMEM;
}
