/* SPDX-License-Identifier: MIT */
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "audio_port.h"
#include "aster_idf_audio.h"
#include "aster_platform.h"
#include <string.h>
#include <stdatomic.h>
const struct device aster_idf_gpio_device={.role=3};
static const struct device i2c_device={.role=1},i2s_device={.role=2};
static i2c_master_dev_handle_t codec;
static i2s_chan_handle_t tx,rx;
static bool tx_on,rx_on;
static struct i2s_config tx_config,rx_config;
static bool available;
static atomic_bool record_cancelled;
void aster_idf_audio_cancel_record(bool cancel) {atomic_store(&record_cancelled,cancel);}
bool aster_idf_audio_record_cancelled(void) {return atomic_load(&record_cancelled);}
int aster_idf_audio_start(void) {uint8_t dac,adc;int r=aster_plat_audio_probe(&dac,&adc);available=r==0;return r;}
bool aster_idf_audio_available(void) {return available;}
static int error(esp_err_t r)
{return r==ESP_OK?0:r==ESP_ERR_TIMEOUT?-ETIMEDOUT:r==ESP_ERR_NO_MEM?-ENOMEM:r==ESP_ERR_INVALID_ARG?-EINVAL:-EIO;}
const struct device *aster_idf_audio_i2c(void)
{
    if(codec)return &i2c_device;
    i2c_master_bus_handle_t bus;
    if(i2c_master_get_bus_handle(1,&bus)!=ESP_OK)return NULL;
    i2c_device_config_t config={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=0x18,.scl_speed_hz=100000};
    if(i2c_master_bus_add_device(bus,&config,&codec)!=ESP_OK)return NULL;
    return &i2c_device;
}
const struct device *aster_idf_audio_i2s(void) {return &i2s_device;}
bool device_is_ready(const struct device *d) {return d && (d->role!=1 || codec);}
int gpio_is_ready_dt(const struct gpio_dt_spec *p) {return p && p->port && GPIO_IS_VALID_OUTPUT_GPIO((int)p->pin);}
int gpio_pin_configure_dt(const struct gpio_dt_spec *p,int mode)
{
    (void)mode;gpio_config_t config={.pin_bit_mask=1ULL<<p->pin,.mode=GPIO_MODE_OUTPUT};
    int r=error(gpio_config(&config));return r?r:error(gpio_set_level(p->pin,0));
}
int gpio_pin_set_dt(const struct gpio_dt_spec *p,int value) {return error(gpio_set_level(p->pin,value));}
int i2c_reg_write_byte(const struct device *d,uint8_t address,uint8_t reg,uint8_t value)
{
    (void)d;if(address!=0x18 || !codec)return -ENODEV;
    uint8_t data[]={reg,value};return error(i2c_master_transmit(codec,data,2,500));
}
int i2c_reg_read_byte(const struct device *d,uint8_t address,uint8_t reg,uint8_t *value)
{(void)d;return address!=0x18 || !codec?-ENODEV:error(i2c_master_transmit_receive(codec,&reg,1,value,1,500));}
static void shutdown(void)
{
    if(tx_on)i2s_channel_disable(tx);
    if(rx_on)i2s_channel_disable(rx);
    tx_on=rx_on=false;
    if(tx)i2s_del_channel(tx);
    if(rx)i2s_del_channel(rx);
    tx=rx=NULL;memset(&tx_config,0,sizeof(tx_config));memset(&rx_config,0,sizeof(rx_config));
}
int i2s_configure(const struct device *d,enum i2s_dir dir,const struct i2s_config *config)
{
    (void)d;
    if(!config->frame_clk_freq){shutdown();return 0;}
    if(config->word_size!=16 || config->channels!=2 || !config->mem_slab || !config->block_size)return -EINVAL;
    if(dir==I2S_DIR_RX) {
        if(!rx || config->frame_clk_freq!=tx_config.frame_clk_freq)return -EINVAL;
        rx_config=*config;return 0;
    }
    if(dir!=I2S_DIR_TX)return -EINVAL;
    shutdown();
    i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER);
    channel.dma_desc_num=6;channel.dma_frame_num=128;channel.auto_clear_after_cb=true;
    esp_err_t r=i2s_new_channel(&channel,&tx,&rx);
    i2s_std_config_t standard={
        .clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(config->frame_clk_freq),
        .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),
        .gpio_cfg={.mclk=13,.bclk=12,.ws=10,.dout=9,.din=11},
    };
    if(r==ESP_OK)r=i2s_channel_init_std_mode(tx,&standard);
    if(r==ESP_OK)r=i2s_channel_init_std_mode(rx,&standard);
    if(r!=ESP_OK){shutdown();return error(r);}
    tx_config=*config;return 0;
}
int i2s_trigger(const struct device *d,enum i2s_dir dir,int trigger)
{
    (void)d;
    if(trigger==I2S_TRIGGER_DRAIN) {
        if(tx_on && tx_config.frame_clk_freq)
            vTaskDelay(pdMS_TO_TICKS((6u*128u*1000u+tx_config.frame_clk_freq-1)/tx_config.frame_clk_freq+2));
        return 0;
    }
    if(trigger==I2S_TRIGGER_DROP || trigger==I2S_TRIGGER_PREPARE) {
        if((dir&I2S_DIR_TX) && tx_on){i2s_channel_disable(tx);tx_on=false;}
        if((dir&I2S_DIR_RX) && rx_on){i2s_channel_disable(rx);rx_on=false;}
        return 0;
    }
    if(trigger!=I2S_TRIGGER_START)return -EINVAL;
    esp_err_t r=ESP_OK;
    if((dir&I2S_DIR_RX) && !rx_on){r=rx?i2s_channel_enable(rx):ESP_ERR_INVALID_STATE;if(r==ESP_OK)rx_on=true;}
    if(r==ESP_OK && (dir&I2S_DIR_TX) && !tx_on){r=tx?i2s_channel_enable(tx):ESP_ERR_INVALID_STATE;if(r==ESP_OK)tx_on=true;}
    return error(r);
}
int i2s_write(const struct device *d,void *block,size_t size)
{
    (void)d;if(!tx || !tx_config.mem_slab)return -ENODEV;
    size_t wrote=0;esp_err_t r=tx_on?i2s_channel_write(tx,block,size,&wrote,tx_config.timeout):
        i2s_channel_preload_data(tx,block,size,&wrote);
    if(r!=ESP_OK)return error(r);
    if(wrote!=size)return -EIO;
    k_mem_slab_free(tx_config.mem_slab,block);return 0;
}
int i2s_read(const struct device *d,void **block,size_t *size)
{
    (void)d;if(!rx_on || !rx_config.mem_slab)return -ENODEV;
    int r=k_mem_slab_alloc(rx_config.mem_slab,block,pdMS_TO_TICKS(rx_config.timeout));if(r)return r;
    esp_err_t status=i2s_channel_read(rx,*block,rx_config.block_size,size,rx_config.timeout);
    if(status!=ESP_OK){k_mem_slab_free(rx_config.mem_slab,*block);*block=NULL;return error(status);}
    return 0;
}
