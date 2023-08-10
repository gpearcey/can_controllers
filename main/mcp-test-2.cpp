/**
 * @file main.cpp
 * 
 * @brief Contains FreeRTOS tasks and a pthread for sending and receiving messages between WASM app and CAN controllers.
 * @todo convert unessesary ESP_LOGI messages to ESP_LOGD
*/

/** 
 * @mainpage Can Controllers Documentation
 * 
 * This is the documentation for the firmware on the esp32 boards for the NMEA T connector project. 
 * These pages contain class and function descriptions. For installation and more detailed instructions, please visit the
 * <a href="https://cyberboat.gitbook.io/cyberboat/">project wiki</a> 
 */

#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
//#include "can.h"
//#include "mcp2515_grace.h"
#include "mcp2515.h"
//#include "NMEA2000.h"
//#include "NMEA2000_mcp.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

#include "NMEA_msg.h"
#include <N2kMsg.h>
#include <NMEA2000.h>

#include <N2kMessages.h>
#include "esp_err.h"

#include <string>
#include <iomanip>
#include "esp_system.h"

#define MY_ESP_LOG_LEVEL                ESP_LOG_DEBUG // the log level for this file

//#define PIN_NUM_MISO 2
//#define PIN_NUM_MOSI 7
//#define PIN_NUM_CLK  6
//#define PIN_NUM_CS   16
//#define PIN_NUM_INT 4
//#define PIN_NUM_INTERRUPT 4


// Tag for ESP logging
static const char* TAG_MCP = "MCP";


//can_frame can_frame_r;

bool interrupt = false;



//bool SPI_Init(void)
//{
//
//	printf("Hello from SPI_Init!\n\r");
//	esp_err_t ret;
//	//Configuration for the SPI bus
//	//spi_device_handle_t spi;
//	spi_bus_config_t bus_cfg={};
//    bus_cfg.miso_io_num=PIN_NUM_MISO;
//    bus_cfg.mosi_io_num=PIN_NUM_MOSI;	
//	bus_cfg.sclk_io_num=PIN_NUM_CLK;
//    bus_cfg.quadwp_io_num=-1;
//    bus_cfg.quadhd_io_num=-1;
//    bus_cfg.max_transfer_sz=16*320*2+8;
//
//
//	// Define MCP2515 SPI device configuration
//	spi_device_interface_config_t dev_cfg = {};
//	dev_cfg.mode = 0; // (0,0)
//	dev_cfg.clock_speed_hz=10*1000*1000; 
//	dev_cfg.spics_io_num = PIN_NUM_CS;
//	dev_cfg.queue_size = 1024;
//	dev_cfg.pre_cb=NULL;
//	
//
//	// Initialize SPI bus
//	ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
//	ESP_ERROR_CHECK(ret);
//
//    // Add MCP2515 SPI device to the bus
//    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &MCP2515_Object->spi);
//    ESP_ERROR_CHECK(ret);
//
//    return true;
//}

/**
 * \brief Creates a NMEA_msg object and adds it to.data the received messages queue
 * 
 * @todo handle out of range data
 * \param N2kMsg Reference to the N2KMs being handled
 * \return void
 */
void HandleNMEA2000Msg(const tN2kMsg &N2kMsg) {
  if (N2kMsg.Source == 14){
    ESP_LOGD("MCP_TEST", "source is 14");
    return;
  }
  ESP_LOGD("MCP_TEST", "Message Handler called");
  NMEA_msg msg;
  msg.controller_number = 0;
  msg.priority = N2kMsg.Priority;
  msg.PGN = N2kMsg.PGN;
  msg.source = N2kMsg.Source;
  msg.data_length_bytes = N2kMsg.DataLen;
  size_t size = sizeof(N2kMsg.Data) / sizeof(N2kMsg.Data[0]);
  // Perform the conversion with range checking
  for (size_t i = 0; i < size; i++) {
      if (N2kMsg.Data[i] <= static_cast<unsigned char>(CHAR_MAX)) {
          msg.data[i] = static_cast<signed char>(N2kMsg.Data[i]);
      } else {
          // Handle out-of-range value
          //msg.data[i] = /* Your desired behavior for out-of-range values */;
          ESP_LOGE("Message Handle", "data out of range for signed array");
      }
  }
}

extern "C" int app_main(void)
{
    esp_log_level_set(TAG_MCP, MY_ESP_LOG_LEVEL);
        printf("Hello from app_main!\n");
	
	spi_device_handle_t spi;
	
	struct can_frame frame;
	

	/* NMEA test*/
	//tNMEA2000_mcp NMEA2000(&spi,16,MCP_8MHZ,10,50);
    //NMEA2000.SetN2kCANMsgBufSize(8);
    //NMEA2000.SetN2kCANReceiveFrameBufSize(250);
    //NMEA2000.EnableForward(false);               
//
    //NMEA2000.SetMsgHandler(HandleNMEA2000Msg);
    //NMEA2000.SetMode(tNMEA2000::N2km_ListenAndSend);
    //
    //NMEA2000.Open();
//
	//
//
	//while(1){
	//	NMEA2000.ParseMessages(); // Calls message handle whenever a message is available
    //
	//	vTaskDelay(10); 
	//}


	/* SPI test */

	MCP2515 NMEA2000(&spi);
	NMEA2000.reset();
	NMEA2000.setBitrate(CAN_250KBPS,MCP_8MHZ);
	NMEA2000.setNormalMode();
	
	frame.can_id = 0x89f80202;
	frame.can_dlc = 8;
	frame.data[0] = 0x01;
	frame.data[1] = 0x02;
	frame.data[2] = 0x03;
	frame.data[3] = 0x04;
	frame.data[4] = 0x05;
	frame.data[5] = 0x06;
	frame.data[6] = 0x07;
	frame.data[7] = 0x08;

	
	while(1){
		if (NMEA2000.sendMessage( MCP2515::TXB1,&frame)==0)
		{
			printf("sucessfully sent message \n");
		}
	
		vTaskDelay(10); 
  		//if (NMEA2000.readMessage(&frame) == MCP2515::ERROR_OK) {
  		//        // frame contains received message
  		//        printf("ID: %lx \n", frame.can_id);
  		//        printf("DATA %x ", frame.data[0]);
		//		printf("DATA %x ", frame.data[1]);
		//		printf("DATA %x \n", frame.data[2]);
		//		//printf("DATA %x ", frame.data[3]);
		//		//printf("DATA %x ", frame.data[4]);
		//		//printf("DATA %x ", frame.data[5]);
		//		//printf("DATA %x ", frame.data[6]);
		//		//printf("DATA %x \n", frame.data[7]);
  		//}	

	}

    return 0;
};