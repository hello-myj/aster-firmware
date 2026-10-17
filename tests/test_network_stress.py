"""The acceptance APP must recover only pre-send connect timeouts, never data faults."""
import unittest
from test_waveshare import ROOT, run_c


class NetworkStress(unittest.TestCase):
    def test_connect_timeout_backoff_and_corrupt_payload_gate(self):
        text = (ROOT/'tools/validation/network-stress/src/network.inc').read_text()
        function = text[text.index('static void net_tcp('):text.index('static void net_http(')]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <aster/network.h>
#include <aster/error.h>
#define NCALL(x) (x)
#define ASTER_LOG_WARN 2
#define aster_logf(...) ((void)0)
static const char net_host[]="127.0.0.1";
static int32_t nh_tcp;
static uint32_t n_tcp_closing,n_tcp_retry_at,n_tcp_timeouts,n_tcp_consecutive,n_tcp_recovered;
static uint32_t n_opened,n_tx,n_rx,n_hstart,n_bytes,n_tcp,n_elapsedmax;
static uint8_t n_input[2048];
static aster_network_status_t next_status;
static int failed,opens,closes,recv_result=ASTER_EAGAIN;
static void net_fail(const char *where,int error){(void)where;assert(error<0);failed++;}
static int net_retry(int rc){return rc==ASTER_EAGAIN||rc==ASTER_ENOSPC||rc==ASTER_EBUSY;}
static void net_close_one(int32_t *h,unsigned kind){assert(kind==1);*h=0;closes++;}
int32_t aster_network_open(uint32_t kind,const char *host,uint32_t len,uint32_t port,uint32_t timeout){
 assert(kind==ASTER_NETWORK_TCP&&host&&len&&port==17780&&timeout==10000);opens++;return 99;}
int32_t aster_network_status(uint32_t h,aster_network_status_t *s,uint32_t size){assert(h&&size==sizeof(*s));*s=next_status;return 0;}
int32_t aster_network_send(uint32_t h,const void *data,uint32_t len,const char *host,uint32_t hostlen,uint32_t port){
 assert(h&&data&&!host&&!hostlen&&!port);return len;}
int32_t aster_network_recv(uint32_t h,void *out,uint32_t cap,aster_network_packet_t *p,uint32_t size){
 assert(h&&cap==512&&p&&size==sizeof(*p));if(recv_result>0)memset(out,7,recv_result);return recv_result;}
''' + function + r'''
int main(void){
 nh_tcp=1;next_status.state=ASTER_NETWORK_FAILED;next_status.phase=ASTER_NETWORK_PHASE_CONNECT;next_status.error=ASTER_ETIMEDOUT;
 net_tcp(100);assert(!nh_tcp&&n_tcp_timeouts==1&&n_tcp_consecutive==1&&!failed&&closes==1);
 net_tcp(500);assert(!opens); /* bounded backoff, no reconnect busy loop */
 nh_tcp=2;n_tx=n_rx=512;next_status.state=ASTER_NETWORK_READY;recv_result=0;
 net_tcp(1200);assert(n_tcp==1&&n_tcp_recovered==1&&!n_tcp_consecutive&&!failed&&!nh_tcp);
 next_status.state=ASTER_NETWORK_FAILED;n_tx=n_rx=0;
 for(int i=0;i<3;i++){nh_tcp=i+3;net_tcp(3000+i*2000);}
 assert(failed==1&&n_tcp_timeouts==4&&n_tcp_consecutive==3); /* does not hide permanent failure */
 failed=0;nh_tcp=9;n_tcp_closing=0;n_tx=512;next_status.phase=ASTER_NETWORK_PHASE_TX;
 net_tcp(10000);assert(failed==1&&n_tcp_timeouts==4); /* never replay potentially delivered data */
 failed=0;nh_tcp=10;n_tx=512;n_rx=0;next_status.state=ASTER_NETWORK_READY;recv_result=1;
 net_tcp(11000);assert(failed==1&&n_tcp_timeouts==4); /* corrupt payload still fails */
 return 0;
}
''')
