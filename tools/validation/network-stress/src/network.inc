/* SPDX-License-Identifier: MIT. Bounded SDK-only concurrent acceptance workload. */
#ifndef N4_HOST
#define N4_HOST "127.0.0.1"
#endif
#ifndef N4_FAST
#define N4_FAST 0
#endif
static const char net_host[]=N4_HOST,net_topic[]="aster/n4/echo",net_client[]="aster-n4-stress";
static int32_t nh_tcp,nh_udp,nh_http,nh_mqtt,n_operation;
static uint32_t n_start,n_phase,n_switch,n_wait,n_opened,n_closed,n_http,n_tcp,n_udp,n_mqtt,n_cancel;
static uint32_t n_calls,n_callmax,n_bins[8],n_bytes,n_elapsedmax,n_errors,n_hstart,n_readymax;
static uint32_t n_tcp_closing,n_http_closing,n_udp_retry,n_udp_retries,n_udp_stale;
static uint32_t n_tcp_timeouts,n_tcp_recovered,n_tcp_consecutive,n_tcp_retry_at;
static uint32_t n_tx,n_rx,n_udp_wait,n_udp_at,n_udp_seq,n_http_rx,n_http_seq,n_stage,n_payload_n,n_mqtt_at;
static uint8_t n_input[2048],n_datagram[64];
static char n_url[384],n_payload[64];
static aster_mqtt_message_t n_message;
static void net_measure(uint32_t before){uint32_t t=aster_time_now_ms()-before;n_calls++;if(t>n_callmax)n_callmax=t;unsigned b=0;while(b<7&&t>(1u<<b)-1u)b++;n_bins[b]++;}
#define NCALL(expr) ({uint32_t _n_before=aster_time_now_ms();int32_t _n_rc=(expr);net_measure(_n_before);_n_rc;})
static void net_fail(const char *where,int32_t rc){n_errors++;aster_logf(ASTER_LOG_ERROR,"N4NET FAIL %s=%d phase=%u",where,rc,n_phase);aster_app_exit(1);}
static int net_retry(int32_t rc){return rc==ASTER_EAGAIN||rc==ASTER_ENOSPC||rc==ASTER_EBUSY;}
static void net_close_one(int32_t *h,unsigned kind){if(*h<=0)return;int rc=kind==1?NCALL(aster_network_close(*h)):kind==2?NCALL(aster_http_close(*h)):NCALL(aster_mqtt_close(*h));if(rc==ASTER_EAGAIN)return;if(rc){net_fail("close",rc);return;}*h=0;n_closed++;}
static void net_stop(void){net_close_one(&nh_tcp,1);net_close_one(&nh_udp,1);net_close_one(&nh_http,2);net_close_one(&nh_mqtt,3);}
static void net_report(void){
 aster_logf(ASTER_LOG_INFO,"N4NET phase=%u opened=%u closed=%u tcp=%u udp=%u http=%u mqtt=%u cancel=%u bytes=%u errors=%u",n_phase,n_opened,n_closed,n_tcp,n_udp,n_http,n_mqtt,n_cancel,n_bytes,n_errors);
 aster_logf(ASTER_LOG_INFO,"N4UDP retries=%u stale=%u",n_udp_retries,n_udp_stale);
 aster_logf(ASTER_LOG_INFO,"N4RECOVER tcp_timeouts=%u tcp_recovered=%u tcp_consecutive=%u",n_tcp_timeouts,n_tcp_recovered,n_tcp_consecutive);
 aster_logf(ASTER_LOG_INFO,"N4LAT calls=%u max_ms=%u done_max_ms=%u ready_max_ms=%u b0=%u b1=%u b2=%u b3=%u b4=%u b5=%u b6=%u b7=%u",n_calls,n_callmax,n_elapsedmax,n_readymax,n_bins[0],n_bins[1],n_bins[2],n_bins[3],n_bins[4],n_bins[5],n_bins[6],n_bins[7]);
}
static void net_init(void){n_start=aster_time_now_ms();n_wait=n_start;}
static void net_udp(uint32_t now){
 int rc;
 if(!nh_udp){rc=NCALL(aster_network_open(ASTER_NETWORK_UDP,0,0,0,0));if(net_retry(rc))return;if(rc<0){net_fail("udp open",rc);return;}nh_udp=rc;n_opened++;n_udp_wait=n_udp_retry=0;}
 if(!n_udp_wait&&now-n_udp_at>=200){for(unsigned i=0;i<64;i++)n_datagram[i]=(uint8_t)(n_udp_seq+i);memcpy(n_datagram,&n_udp_seq,4);rc=NCALL(aster_network_send(nh_udp,n_datagram,64,net_host,sizeof(net_host)-1,17781));if(rc==ASTER_EAGAIN)return;if(rc!=64){net_fail("udp send",rc);return;}n_udp_wait=1;n_udp_at=now;}
 if(n_udp_wait){aster_network_packet_t p;rc=NCALL(aster_network_recv(nh_udp,n_input,64,&p,sizeof(p)));if(rc==ASTER_EAGAIN){if(now-n_udp_at>5000){if(n_udp_retry++>=3)net_fail("udp timeout",ASTER_ETIMEDOUT);else{n_udp_retries++;n_udp_wait=0;n_udp_at=now-200;}}return;}if(rc==64&&p.port==17781){uint32_t got;memcpy(&got,n_input,4);if(got!=n_udp_seq){n_udp_stale++;return;}}if(rc!=64||p.port!=17781||memcmp(n_input,n_datagram,64)){net_fail("udp content",rc);return;}n_udp_wait=n_udp_retry=0;n_udp_seq++;n_udp++;n_bytes+=64;}
}
static void net_tcp(uint32_t now){
 int rc;
 if(n_tcp_closing){net_close_one(&nh_tcp,1);if(!nh_tcp)n_tcp_closing=0;return;}
 if(!nh_tcp&&now<n_tcp_retry_at)return;
 if(!nh_tcp){rc=NCALL(aster_network_open(ASTER_NETWORK_TCP,net_host,sizeof(net_host)-1,17780,10000));if(net_retry(rc))return;if(rc<0){net_fail("tcp open",rc);return;}nh_tcp=rc;n_opened++;n_tx=n_rx=0;n_hstart=now;}
 aster_network_status_t s;rc=NCALL(aster_network_status(nh_tcp,&s,sizeof(s)));if(rc==ASTER_EAGAIN)return;if(rc||s.state>=ASTER_NETWORK_FAILED){
  int error=rc?rc:s.error;
  aster_logf(ASTER_LOG_WARN,"N4TCP state=%u phase=%u port=%u tx=%u rx=%u error=%d",s.state,s.phase,s.local_port,n_tx,n_rx,error);
  /* A connect timeout is a valid network outcome, not a corrupt payload.
   * Exercise APP recovery without retrying data that might already be sent. */
  if(!rc&&error==ASTER_ETIMEDOUT&&s.phase==ASTER_NETWORK_PHASE_CONNECT&&!n_tx&&!n_rx){
   n_tcp_timeouts++;n_tcp_consecutive++;
   if(n_tcp_consecutive>=3){net_fail("tcp consecutive connect timeouts",error);return;}
   n_tcp_closing=1;n_tcp_retry_at=now+1000;net_close_one(&nh_tcp,1);if(!nh_tcp)n_tcp_closing=0;return;
  }
  net_fail("tcp state",error);return;
 }if(s.state<ASTER_NETWORK_READY)return;
 if(n_tx<512){uint32_t cap=512-n_tx;for(unsigned i=0;i<cap;i++)n_input[i]=(uint8_t)((n_tx+i)%251);rc=NCALL(aster_network_send(nh_tcp,n_input,cap,0,0,0));if(rc==ASTER_EAGAIN)return;if(rc<0){net_fail("tcp send",rc);return;}n_tx+=rc;}
 aster_network_packet_t p;rc=NCALL(aster_network_recv(nh_tcp,n_input,512,&p,sizeof(p)));if(rc==ASTER_EAGAIN)return;if(rc<0){net_fail("tcp recv",rc);return;}
 for(int i=0;i<rc;i++)if(n_input[i]!=(uint8_t)((n_rx+i)%251)){net_fail("tcp content",ASTER_ECORRUPT);return;}
 n_rx+=rc;n_bytes+=rc;
 if(!rc){if(n_rx!=512||n_tx!=512){net_fail("tcp EOF",ASTER_ECORRUPT);return;}n_tcp++;n_tcp_recovered+=n_tcp_consecutive;n_tcp_consecutive=0;if(now-n_hstart>n_elapsedmax)n_elapsedmax=now-n_hstart;n_tcp_closing=1;net_close_one(&nh_tcp,1);if(!nh_tcp)n_tcp_closing=0;}
}
static void net_http(uint32_t now,int secure){
 int rc;
 if(n_http_closing){
  if(n_http_closing==1){rc=NCALL(aster_http_cancel(nh_http));if(rc==ASTER_EAGAIN)return;if(rc){net_fail("http cancel",rc);return;}n_cancel++;n_http_closing=2;}
  net_close_one(&nh_http,2);if(!nh_http)n_http_closing=0;return;
 }
 if(!nh_http){
  aster_snprintf(n_url,sizeof(n_url),"http%s://%s:%u/fixed?n=%u",secure?"s":"",net_host,secure?9443:9080,N4_FAST?64:16384);
  rc=NCALL(aster_http_request(ASTER_HTTP_METHOD_GET,n_url,strlen(n_url),0,0,0,0,20000,65536));if(net_retry(rc))return;if(rc<0){net_fail("http start",rc);return;}
  nh_http=rc;n_opened++;n_http_rx=0;n_hstart=now;
  if((++n_http_seq%10u)==0){n_http_closing=1;return;}
 }
 for(unsigned pass=0;pass<2;pass++){
  rc=NCALL(aster_http_read(nh_http,n_input,sizeof(n_input)));if(rc==ASTER_EAGAIN)return;if(rc<0){net_fail("http read",rc);return;}
  for(int i=0;i<rc;i++)if(n_input[i]!=(uint8_t)((n_http_rx+i)%251)){net_fail("http content",ASTER_ECORRUPT);return;}
  n_http_rx+=rc;n_bytes+=rc;
  if(!rc){aster_http_status_t s;rc=NCALL(aster_http_status(nh_http,&s,sizeof(s)));if(rc==ASTER_EAGAIN)return;if(rc||s.status!=200||n_http_rx!=(N4_FAST?64u:16384u)){net_fail("http done",rc?rc:ASTER_ECORRUPT);return;}
   n_http++;if(now-n_hstart>n_elapsedmax)n_elapsedmax=now-n_hstart;n_http_closing=3;net_close_one(&nh_http,2);if(!nh_http)n_http_closing=0;return;}
 }
}
static void net_mqtt(uint32_t now){
 int rc;
 if(!nh_mqtt){aster_mqtt_options_t o={.size=sizeof(o),.version=1,.flags=ASTER_MQTT_FLAG_TLS|ASTER_MQTT_FLAG_RECONNECT,.port=8883,.timeout_ms=20000,.keepalive_s=5,
 .host={(uint32_t)(uintptr_t)net_host,sizeof(net_host)-1},.client_id={(uint32_t)(uintptr_t)net_client,sizeof(net_client)-1}};
 rc=NCALL(aster_mqtt_connect(&o,sizeof(o)));if(net_retry(rc))return;if(rc<0){net_fail("mqtt open",rc);return;}nh_mqtt=rc;n_opened++;n_stage=0;n_operation=0;n_mqtt_at=now;}
 aster_mqtt_status_t s;rc=NCALL(aster_mqtt_status(nh_mqtt,&s,sizeof(s)));if(rc==ASTER_EAGAIN)return;if(rc||s.state>=ASTER_MQTT_STATE_FAILED){net_fail("mqtt state",rc?rc:s.error);return;}if(s.state!=ASTER_MQTT_STATE_READY)return;
 if(!n_stage&&now-n_mqtt_at>n_readymax)n_readymax=now-n_mqtt_at;
 if(n_operation){int32_t error;rc=NCALL(aster_mqtt_operation(nh_mqtt,n_operation,&error));if(rc==ASTER_EAGAIN)return;if(rc||error){net_fail("mqtt operation",rc?rc:error);return;}n_operation=0;n_stage++;}
 if(n_stage==0){rc=NCALL(aster_mqtt_subscribe(nh_mqtt,net_topic,sizeof(net_topic)-1,1));if(net_retry(rc))return;if(rc<0){net_fail("mqtt sub",rc);return;}n_operation=rc;return;}
 if(n_stage==1){if(now-n_mqtt_at<200)return;n_payload_n=aster_snprintf(n_payload,sizeof(n_payload),"n4-%u",n_mqtt);rc=NCALL(aster_mqtt_publish(nh_mqtt,net_topic,sizeof(net_topic)-1,n_payload,n_payload_n,1,0));if(rc==ASTER_EAGAIN)return;if(rc<0){net_fail("mqtt publish",rc);return;}n_operation=rc;n_mqtt_at=now;return;}
 if(now-n_mqtt_at>10000){net_fail("mqtt echo timeout",ASTER_ETIMEDOUT);return;}
 rc=NCALL(aster_mqtt_recv(nh_mqtt,&n_message,sizeof(n_message)));if(rc==ASTER_EAGAIN)return;if(rc||n_message.length!=n_payload_n||n_message.topic_length!=sizeof(net_topic)-1||memcmp(n_message.topic,net_topic,sizeof(net_topic)-1)||memcmp(n_message.data,n_payload,n_payload_n)){net_fail("mqtt content",rc?rc:ASTER_ECORRUPT);return;}n_mqtt++;n_bytes+=n_payload_n;n_stage=1;
}
static void net_tick(uint32_t now){
 if(N4_FAST){n_phase=1;net_http(now,0);if(n_http>=100&&n_cancel>=10&&!nh_http){net_report();aster_logf(ASTER_LOG_INFO,"N4FAST PASS http=%u cancel=%u",n_http,n_cancel);aster_app_exit(0);}return;}
 unsigned wanted=((now-n_start)/20000u)%3u;
 if(n_switch||wanted!=n_phase){n_switch=1;net_stop();if(nh_tcp||nh_udp||nh_http||nh_mqtt)return;n_tcp_closing=n_http_closing=0;n_phase=wanted;n_wait=now;n_switch=0;return;}
 if(now-n_wait<250)return;
 if(n_phase==0){net_udp(now);net_tcp(now);}
 else if(n_phase==1){net_udp(now);net_http(now,1);}
 else {net_mqtt(now);net_http(now,0);}
}
