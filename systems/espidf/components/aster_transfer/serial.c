/* SPDX-License-Identifier: MIT */
#include "transfer.h"
#include "aster_storage.h"
#include "aster_sysinfo.h"
#include <aster/system.h>
#include "esp_console.h"
#include "driver/uart.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
static int ping(int argc,char **argv)
{
    (void)argc;(void)argv;uint64_t caps=0;
    for(unsigned bit=0;bit<64;bit++)if(aster_sysinfo_has_feature(bit))caps|=1ULL<<bit;
    printf("\nPONG sd=%d product=aster target=riscv abi=%u.%u caps=%llx serial=shared transfer=packages\n",
           aster_storage_mounted()?1:0,ASTER_ABI_MAJOR,ASTER_ABI_MINOR,(unsigned long long)caps);
    fflush(stdout);return 0;
}
static int receive(void *context,uint8_t *data,size_t size)
{
    (void)context;int n=uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM,data,size,pdMS_TO_TICKS(30000));
    return n>0?n:-ETIMEDOUT;
}
static void reply(void *context,const char *line)
{(void)context;printf("\n%s\n",line);fflush(stdout);}
static int put(int argc,char **argv)
{
    /* The SDK sender waits for ACK before sending a byte. While this handler
     * runs, REPL isn't reading UART; read the driver directly so CR/LF and
     * binary NUL never pass through stdio's terminal transformations. */
    if(argc!=4 && argc!=5){reply(NULL,"ERR -22");return 0;}
    char path[192];
    if(aster_transfer_package_path(argv[1],path,sizeof(path))) {
        reply(NULL,"ERR -1");return 0;
    }
    if(!aster_storage_mounted()){reply(NULL,"ERR -19");return 0;}
    uint32_t values[3]={0};
    for(int i=2;i<argc;i++) {
        char *end;errno=0;unsigned long value=strtoul(argv[i],&end,i==3?16:10);
        if(!argv[i][0] || argv[i][0]=='-' || *end || errno || value>UINT32_MAX){reply(NULL,"ERR -22");return 0;}
        values[i-2]=(uint32_t)value;
    }
    const aster_transfer_io_t io={receive,reply,NULL};
    int result=aster_transfer_receive(path,values[0],values[1],values[2],&io);
    if(result)uart_flush_input(CONFIG_ESP_CONSOLE_UART_NUM);
    return 0;
}
int aster_idf_transfer_register(void)
{
    const esp_console_cmd_t probe={.command="ASTERPING",.help="Probe ASTERFS shared UART",.func=ping};
    const esp_console_cmd_t upload={.command="ASTERPUT",.help="Receive an ASTP package using ASTERFS 3",.func=put};
    esp_err_t result=esp_console_cmd_register(&probe);
    return result==ESP_OK?esp_console_cmd_register(&upload):result;
}
