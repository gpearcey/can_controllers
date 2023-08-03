#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"

#define PIN_NUM_MISO 2
#define PIN_NUM_MOSI 7
#define PIN_NUM_CLK  6
#define PIN_NUM_CS   16

#define DECODE_MODE_REG     0x09
#define INTENSITY_REG       0x0A
#define SCAN_LIMIT_REG      0x0B
#define SHUTDOWN_REG        0x0C
#define DISPLAY_TEST_REG    0x0F
#define PARALLEL_LINES 16





//static void write_reg(uint8_t reg, uint8_t value) {
//    uint8_t tx_data[2] = { reg, value };
//
//    spi_transaction_t t = {};
//    t.tx_buffer = tx_data;
//    t.length = 2 * 8;
//    
//
//    ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &t));
//}
//
//static void set_row(uint8_t row_index) {
//  write_reg(row_index + 1, 0xFF);
//}
//
//static void set_col(uint8_t col_index) {
//  for (int i = 0; i < 8; i++) {
//    write_reg(i + 1, 0x01 << col_index);
//  }
//}
//
//static void clear(void) {
//  for (int i = 0; i < 8; i++) {
//    write_reg(i + 1, 0x00);
//  }
//}
//
//static void max7219_init() {
//    write_reg(DISPLAY_TEST_REG, 0);
//    write_reg(SCAN_LIMIT_REG, 7);
//    write_reg(DECODE_MODE_REG, 0);
//    write_reg(SHUTDOWN_REG, 1);
//    clear();
//}

extern "C" int app_main(void)
{
    esp_err_t ret;
    spi_device_handle_t spi;
    spi_bus_config_t buscfg={};
    buscfg.miso_io_num=PIN_NUM_MISO;
    buscfg.mosi_io_num=PIN_NUM_MOSI;
    buscfg.sclk_io_num=PIN_NUM_CLK;
    buscfg.quadwp_io_num=-1;
    buscfg.quadhd_io_num=-1;
    buscfg.max_transfer_sz=16*320*2+8;
    
    spi_device_interface_config_t devcfg={};
    devcfg.clock_speed_hz=10*1000*1000;           //Clock out at 10 MHz
    devcfg.mode=0;                                //SPI mode 0
    devcfg.spics_io_num=PIN_NUM_CS;               //CS pin
    devcfg.queue_size=7;                          //We want to be able to queue 7 transactions at a time
    devcfg.pre_cb=NULL;//lcd_spi_pre_transfer_callback,  //Specify pre-transfer callback to handle D/C line

    //Initialize the SPI bus
    ret=spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);
    //Attach the LCD to the SPI bus
    ret=spi_bus_add_device(SPI2_HOST, &devcfg, &spi);
    ESP_ERROR_CHECK(ret);
    //max7219_init();

    while (1) {
        //for (int i = 0; i < 8; i++) {
        //    clear();
        //    set_row(i);
        //    vTaskDelay(1000/portTICK_PERIOD_MS);
        //}
//
        //for (int i = 0; i < 8; i++) {
        //    clear();
        //    set_col(i);
        
        //}
         int x;
        int ypos = 32;
        uint16_t number = 234;
        uint16_t* ptr = &number;
        //Transaction descriptors. Declared static so they're not allocated on the stack; we need this memory even when this
        //function is finished because the SPI driver needs access to it even while we're already calculating the next line.
        static spi_transaction_t trans = {};
        trans.length = 32;
        trans.flags = SPI_TRANS_USE_RXDATA | SPI_TRANS_USE_TXDATA;
        trans.tx_data[0] = 1;
        trans.tx_data[1] = 1;
        trans.tx_data[2] = 1U << 3;
        trans.tx_data[3] = 1;
        esp_err_t ret = spi_device_transmit(spi, &trans);
        //vTaskDelay(10/portTICK_PERIOD_MS);


        //In theory, it's better to initialize trans and data only once and hang on to the initialized
        //variables. We allocate them on the stack, so we need to re-init them each call.
        //for (x=0; x<6; x++) {
        //    memset(&trans[x], 0, sizeof(spi_transaction_t));
        //    trans[x].length=8*4;
        //    trans[x].user=(void*)1;
        //    trans[x].flags=SPI_TRANS_USE_TXDATA;
        //}
        //trans[0].tx_data[0]=0x2A;           //Column Address Set
        //trans[1].tx_data[0]=0;              //Start Col High
        //trans[1].tx_data[1]=0;              //Start Col Low
        //trans[1].tx_data[2]=(320)>>8;       //End Col High
        //trans[1].tx_data[3]=(320)&0xff;     //End Col Low
        //trans[2].tx_data[0]=0x2B;           //Page address set
        //trans[3].tx_data[0]=ypos>>8;        //Start page high
        //trans[3].tx_data[1]=ypos&0xff;      //start page low
        //trans[3].tx_data[2]=(ypos+PARALLEL_LINES)>>8;    //end page high
        //trans[3].tx_data[3]=(ypos+PARALLEL_LINES)&0xff;  //end page low
        //trans[4].tx_data[0]=0x2C;           //memory write
        //trans[5].tx_buffer=ptr;        //finally send the line data
        //trans[5].length=320*2*8*PARALLEL_LINES;          //Data length, in bits
        //trans[5].flags=0; //undo SPI_TRANS_USE_TXDATA flag

        //Queue all transactions.
        //for (x=0; x<6; x++) {
        //    ret=spi_device_queue_trans(spi, &trans[x], portMAX_DELAY);
        //    assert(ret==ESP_OK);
        //}

    }

    return 0;
}