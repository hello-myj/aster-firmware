/* SPDX-License-Identifier: MIT */
#include "aster_platform.h"
#include "wifi_service.h"
#include <string.h>
#include <errno.h>
static int error(int r)
{
    switch(r){case 0:return 0;case ASTER_EBUSY:return -EBUSY;case ASTER_ENOENT:return -ENOENT;
    case ASTER_EAGAIN:return -EAGAIN;case ASTER_EINVAL:return -EINVAL;case ASTER_ESTATE:return -ENETDOWN;
    case ASTER_ETIMEDOUT:return -ETIMEDOUT;default:return -EIO;}
}
int aster_plat_wifi_status(aster_wifi_status_t *out)
{
    if(!out)return -EINVAL;
    memset(out,0,sizeof(*out));
    aster_wifi_info_t info;int r=aster_idf_wifi_snapshot(&info);if(r)return error(r);
    memcpy(out->ssid,info.ssid,sizeof(out->ssid));memcpy(out->ip,info.ipv4,sizeof(out->ip));
    out->rssi=info.rssi;out->state=info.state;out->security=info.security;
    out->state_txt=info.state==ASTER_WIFI_STATE_COMPLETED?"COMPLETED":"DISCONNECTED";
    out->sec_txt=aster_plat_wifi_sec_txt(info.security);return 0;
}
const char *aster_plat_wifi_sec_txt(uint8_t security)
{switch(security){case ASTER_WIFI_SECURITY_OPEN:return "OPEN";case ASTER_WIFI_SECURITY_PERSONAL:return "WPA";
case ASTER_WIFI_SECURITY_ENTERPRISE:return "ENTERPRISE";case ASTER_WIFI_SECURITY_WEP:return "WEP";default:return "OTHER";}}
uint32_t aster_plat_wifi_security_class(uint8_t security) {return security;}
uint32_t aster_plat_wifi_state_class(int state) {return state>=0 && state<=10?(uint32_t)state:10;}
int aster_plat_wifi_scan_start(void) {return error(aster_idf_wifi_submit(ASTER_WIFI_OP_SCAN,NULL,NULL));}
int aster_plat_wifi_scan_results(aster_wifi_ap_t *out,int max,int *count)
{
    if(!out || !count || max<1 || max>ASTER_WIFI_AP_MAX)return -EINVAL;
    aster_wifi_ap_info_t rows[ASTER_WIFI_AP_MAX];int n=aster_idf_wifi_results(rows,max);if(n<0)return error(n);
    for(int i=0;i<n;i++){memcpy(out[i].ssid,rows[i].ssid,33);out[i].rssi=rows[i].rssi;out[i].channel=rows[i].channel;out[i].security=rows[i].security;}
    *count=n;return 0;
}
int aster_plat_wifi_connect_start(const char *ssid,const char *password)
{return error(aster_idf_wifi_submit(ASTER_WIFI_OP_CONNECT,ssid,password?password:""));}
int aster_plat_wifi_disconnect_start(void) {return error(aster_idf_wifi_submit(ASTER_WIFI_OP_DISCONNECT,NULL,NULL));}
int aster_plat_wifi_reconnect_start(void) {return error(aster_idf_wifi_submit(ASTER_WIFI_OP_RECONNECT,NULL,NULL));}
int aster_plat_wifi_forget_start(void) {return error(aster_idf_wifi_submit(ASTER_WIFI_OP_FORGET,NULL,NULL));}
void aster_plat_wifi_operation(uint32_t *op,uint32_t *last,int *result)
{aster_wifi_info_t info;int r=aster_idf_wifi_snapshot(&info);*op=r?0:info.operation;*last=r?0:info.last_operation;*result=r?error(r):error(info.last_result);}
int aster_plat_wifi_last_ssid(char *out,size_t size)
{char ssid[33];int r=aster_idf_wifi_saved(ssid);if(r)return error(r);size_t n=strlen(ssid)+1;if(size<n)return -ENOSPC;memcpy(out,ssid,n);return 0;}
/* AP jobs use the same remote radio worker; credentials never enter LVGL. */
int aster_plat_wifi_hotspot_start(const char *ssid,const char *password)
{return error(aster_idf_wifi_hotspot_submit(true,ssid,password));}
int aster_plat_wifi_hotspot_stop(void) {return error(aster_idf_wifi_hotspot_submit(false,NULL,NULL));}
void aster_plat_wifi_hotspot_status(aster_wifi_hotspot_status_t *out)
{memset(out,0,sizeof(*out));aster_idf_wifi_hotspot_snapshot(out->ssid,&out->enabled,&out->configured);}
