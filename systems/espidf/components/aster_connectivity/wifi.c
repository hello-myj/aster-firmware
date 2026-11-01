/* SPDX-License-Identifier: MIT */
#include "aster_idf_wifi.h"
#include "wifi_service.h"
#include "clock.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_console.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG="aster_wifi";
static esp_netif_t *station;
static int (*app_command)(int,char **);
void aster_idf_set_app_command(int (*callback)(int,char **)) {app_command=callback;}
static void event(void *arg,esp_event_base_t base,int32_t id,void *data)
{
    (void)arg;
    aster_idf_wifi_event(base,id,data);
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP)aster_idf_clock_network_ready();
}
static int command(int argc,char **argv)
{
    if(argc>=2 && (!strcmp(argv[1],"app") || !strcmp(argv[1],"ui")) && app_command) {
        int result=app_command(argc,argv);printf("APP command result=%d\n",result);return result?1:0;
    }
    if (argc<3 || strcmp(argv[1],"wifi")) {
        puts("aster wifi scan|status|disconnect|connect SSID [PASSWORD]; aster app list|run|stop|install|uninstall");return 1;
    }
    int result=ASTER_EINVAL;
    if (!strcmp(argv[2],"scan") && argc==3) result=aster_idf_wifi_submit(ASTER_WIFI_OP_SCAN,NULL,NULL);
    else if (!strcmp(argv[2],"connect") && (argc==4 || argc==5))
        result=aster_idf_wifi_submit(ASTER_WIFI_OP_CONNECT,argv[3],argc==5?argv[4]:"");
    else if (!strcmp(argv[2],"disconnect") && argc==3) result=aster_idf_wifi_submit(ASTER_WIFI_OP_DISCONNECT,NULL,NULL);
    else if (!strcmp(argv[2],"reconnect") && argc==3) result=aster_idf_wifi_submit(ASTER_WIFI_OP_RECONNECT,NULL,NULL);
    else if (!strcmp(argv[2],"forget") && argc==3) result=aster_idf_wifi_submit(ASTER_WIFI_OP_FORGET,NULL,NULL);
    else if (!strcmp(argv[2],"status") && argc==3) {
        aster_wifi_info_t info;result=aster_idf_wifi_snapshot(&info);
        if(!result)printf("state=%lu operation=%lu result=%ld SSID=%s IPv4=%s RSSI=%ld\n",
            (unsigned long)info.state,(unsigned long)info.operation,(long)info.last_result,info.ssid,info.ipv4,(long)info.rssi);
    } else if (!strcmp(argv[2],"results") && argc==3) {
        aster_wifi_ap_info_t aps[ASTER_WIFI_AP_MAX];result=aster_idf_wifi_results(aps,ASTER_WIFI_AP_MAX);
        for(int i=0;i<result;i++)printf("%s RSSI=%ld channel=%lu\n",aps[i].ssid,(long)aps[i].rssi,(unsigned long)aps[i].channel);
        if(result>=0)result=0;
    }
    printf("Wi-Fi command result=%d\n",result);return result?1:0;
}
void aster_idf_network_start(void)
{
    /* Board touch reset (GPIO6, shared with C5 boot) has completed before here.
     * ESP-Hosted resets C5 only via GPIO54. SD card occupies slot 0. */
    esp_err_t r=nvs_flash_init();
    if (r!=ESP_OK) {ESP_LOGE(TAG,"NVS initialization: %s",esp_err_to_name(r));return;}
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    aster_idf_clock_init();
    station=esp_netif_create_default_wifi_sta();
    if (!station) {ESP_LOGE(TAG,"No station interface");return;}
    if(!esp_netif_create_default_wifi_ap()){ESP_LOGE(TAG,"No hotspot interface");return;}
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,event,NULL));
    int service=aster_idf_wifi_service_start(station);
    if(service)ESP_LOGE(TAG,"Wi-Fi service unavailable: %d",service);
    esp_console_repl_t *repl=NULL;
    esp_console_repl_config_t config=ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    /* Package parsing/install paths exceed the default 4 KiB REPL stack. */
    config.task_stack_size=16384;
    config.prompt="aster:~$ ";
    esp_console_dev_uart_config_t uart=ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    r=esp_console_new_repl_uart(&uart,&config,&repl);
    if (r!=ESP_OK) {ESP_LOGE(TAG,"Console unavailable: %s",esp_err_to_name(r));return;}
    const esp_console_cmd_t cmd={.command="aster",.help="Aster device commands",.func=command};
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
