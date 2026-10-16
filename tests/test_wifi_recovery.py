"""Compile the actual connection path; exercise lost DHCP completion and retry."""
from pathlib import Path
import unittest
from test_waveshare import run_c
ROOT = Path(__file__).resolve().parents[1]


class WifiRecovery(unittest.TestCase):
    def test_association_does_not_imply_ipv4(self):
        source = (ROOT / 'platform/esp32s3/wifi.c').read_text()
        helper = source[source.index('static int wifi_wait_ipv4(void)'):
                        source.index('static void wifi_handler(')]
        connect = source[source.index('int aster_plat_wifi_connect('):
                         source.index('int aster_plat_wifi_disconnect(')]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#define ASTER_WIFI_SSID_MAX 32
#define NET_IPV4_ADDR_LEN 16
#define DHCP_TO_MS 15000
#define CONN_TO_MS 35000
#define WIFI_STATE_COMPLETED 1
#define WIFI_STATE_ASSOCIATED 2
#define WIFI_CHANNEL_ANY 0
#define WIFI_FREQ_BAND_2_4_GHZ 0
#define WIFI_MFP_OPTIONAL 0
#define WIFI_SECURITY_TYPE_NONE 0
#define WIFI_SECURITY_TYPE_PSK 1
#define CONN_WAIT_NONE 0
#define CONN_WAIT_CONNECT 1
#define NET_REQUEST_WIFI_CONNECT 1
#define WIFI_PROFILE_VERSION 1
#define WIFI_KV_ID "wifi"
#define WIFI_KV_KEY "profile"
#define K_FOREVER -1
#define K_NO_WAIT 0
#define K_MSEC(x) (x)
#define LOG_WRN(...) ((void)0)
typedef struct { int state;char ssid[33]; } aster_wifi_status_t;
typedef struct { uint32_t version,psk_len;char ssid[33],psk[65]; } wifi_profile_t;
struct wifi_connect_req_params { const uint8_t *ssid,*psk;int ssid_length,psk_length,channel,band,mfp,timeout,security; };
static int wifi_mu,dhcp_done,conn_done,conn_wait_kind,conn_status;
static void *iface;
static char req_ssid[33],req_psk[65];static unsigned req_psk_len;
static struct {int security;} scan_aps[1];
static int associated,radio_success,event_present,event_result,never_ip;
static int restarts,radio_calls,saves;
static int64_t now,ip_at;
static int wifi_ready(void) { return 0; }
static void k_mutex_lock(int *p,int t) {(void)t;assert(!*p);*p=1;}
static void k_mutex_unlock(int *p) {assert(*p);*p=0;}
static void atomic_set(int *p,int n) {*p=n;}
static int64_t k_uptime_get(void) {return now;}
static void fill_ip(char *out,size_t cap) { assert(cap>=16);strcpy(out,associated&&now>=ip_at?"10.0.0.2":""); }
static void net_dhcpv4_restart(void *i) { (void)i;restarts++;ip_at=never_ip?INT64_MAX:now+500; }
static int k_sem_take(int *p,int timeout) {
 if(!timeout)return -EAGAIN;
 if(p==&conn_done&&event_present)return 0;
 now+=timeout;return -EAGAIN;
}
static int aster_plat_wifi_status(aster_wifi_status_t *out) {
 out->state=associated?WIFI_STATE_COMPLETED:0;strcpy(out->ssid,"test-ap");return 0;
}
static int aster_plat_wifi_disconnect(void) {associated=0;return 0;}
static int scan_find(const char *s) {(void)s;return -1;}
static int sta_security(int a,int *b) {*b=a;return 0;}
static int net_mgmt(int op,void *i,void *params,size_t n) {
 (void)op;(void)i;(void)params;(void)n;radio_calls++;associated=radio_success;
 conn_status=event_result;return 0;
}
static int aster_kv_set(const char *id,const char *key,const void *p,size_t n) {
 (void)id;(void)key;(void)p;(void)n;saves++;return 0;
}
''' + helper + connect + r'''
static void reset(void) {
 wifi_mu=associated=restarts=radio_calls=saves=never_ip=event_present=event_result=0;
 now=0;ip_at=INT64_MAX;radio_success=1;
}
int main(void) {
 reset();associated=1;
 assert(!aster_plat_wifi_connect("test-ap","testpass"));
 assert(restarts==1&&radio_calls==0&&saves==0&&now==500);
 reset(); /* Driver associated but completion event did not arrive. */
 assert(!aster_plat_wifi_connect("test-ap","testpass"));
 assert(restarts==1&&radio_calls==1&&saves==1&&now==35500);
 reset();associated=1;never_ip=1;
 assert(aster_plat_wifi_connect("test-ap","testpass")==-ETIMEDOUT);
 assert(restarts==1&&radio_calls==0&&saves==0&&now==15000);
 reset();associated=1;ip_at=0;
 assert(!aster_plat_wifi_connect("test-ap","testpass"));assert(!restarts&&!radio_calls&&!saves);
 reset();radio_success=0;event_present=1;event_result=-EACCES;
 assert(aster_plat_wifi_connect("test-ap","testpass")==-EACCES);
 assert(!restarts&&radio_calls==1&&!saves);
 return 0;
}
''')

    def test_scan_security_and_join_policy_agree(self):
        import re
        source = (ROOT / 'platform/esp32s3/wifi.c').read_text()
        helpers = source[source.index('static int sta_security('):
                         source.index('uint32_t aster_plat_wifi_state_class(')]
        names = sorted(set(re.findall(r'WIFI_SECURITY_TYPE_[A-Z0-9_]+', helpers)))
        prefix = '#include <stdint.h>\n#include <assert.h>\n#include <errno.h>\n'
        prefix += 'enum wifi_security_type {' + ','.join(names) + '};\n'
        classes = ['OPEN', 'PERSONAL', 'ENTERPRISE', 'WEP', 'OTHER']
        prefix += ''.join('#define ASTER_PLAT_WIFI_SECURITY_' + name + ' ' + str(i) + '\n' for i, name in enumerate(classes))
        run_c(prefix + helpers + r'''
int main(void) {
 enum wifi_security_type mode;
 assert(aster_plat_wifi_security_class(WIFI_SECURITY_TYPE_UNKNOWN)==ASTER_PLAT_WIFI_SECURITY_PERSONAL);
 assert(!sta_security(WIFI_SECURITY_TYPE_UNKNOWN,&mode)&&mode==WIFI_SECURITY_TYPE_PSK);
 assert(aster_plat_wifi_security_class(WIFI_SECURITY_TYPE_PSK)==ASTER_PLAT_WIFI_SECURITY_PERSONAL);
 assert(aster_plat_wifi_security_class(WIFI_SECURITY_TYPE_NONE)==ASTER_PLAT_WIFI_SECURITY_OPEN);
 assert(!sta_security(WIFI_SECURITY_TYPE_NONE,&mode)&&mode==WIFI_SECURITY_TYPE_NONE);
 assert(aster_plat_wifi_security_class(WIFI_SECURITY_TYPE_EAP)==ASTER_PLAT_WIFI_SECURITY_ENTERPRISE);
 assert(sta_security(WIFI_SECURITY_TYPE_EAP,&mode)==-ENOTSUP);
 assert(aster_plat_wifi_security_class(WIFI_SECURITY_TYPE_WEP)==ASTER_PLAT_WIFI_SECURITY_WEP);
 assert(sta_security(WIFI_SECURITY_TYPE_WEP,&mode)==-ENOTSUP);
 assert(!sta_security(WIFI_SECURITY_TYPE_SAE,&mode)&&mode==WIFI_SECURITY_TYPE_SAE);
 return 0;
}
''')
