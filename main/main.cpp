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
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "NMEA_msg.h"
#include "esp_log.h"
#include <N2kMsg.h>
#include <NMEA2000_esp32-c6.h> 
#include <NMEA2000.h>
#include <N2kMessages.h>
#include "esp_err.h"
#include "esp_pthread.h"
#include <NMEA2000_mcp.h>

#include "driver/gpio.h"
#include "bi-inc/attr_container.h"

#include "wasm_export.h"
#include "bh_read_file.h"
#include "bh_getopt.h"
#include "bh_platform.h"

#include <queue>
#include <string>
#include <iomanip>
#include <chrono>
#include "driver/spi_master.h"

//WebAssembley App
#include "nmea_attack.h" 

#define NATIVE_STACK_SIZE               (32*1024)
#define NATIVE_HEAP_SIZE                (32*1024)
#define PTHREAD_STACK_SIZE              4096
#define MAX_DATA_LENGTH_BTYES           223
#define MSG_BUFFER_SIZE                     (10 + 223*2) //10 bytes for id, 223*2 bytes for data
#define MODE_BUFFER_SIZE                1 // 1 byte to store modes 0 -> 3
#define MY_ESP_LOG_LEVEL                ESP_LOG_INFO // the log level for this file

#define STATS_TASK_PRIO     tskIDLE_PRIORITY //3
#define STATS_TICKS         pdMS_TO_TICKS(1000)
#define ARRAY_SIZE_OFFSET   5   //Increase this if print_real_time_stats returns ESP_ERR_INVALID_SIZE
#define TX_QUEUE_SIZE       100
#define RX_QUEUE_SIZE       100

#define MCP0_TX             GPIO_NUM_22
#define MCP0_RX             GPIO_NUM_23
#define MCP1_CS             16
#define MCP1_INT            10
#define MCP2_CS             17
#define MCP2_INT            11

#define ESP_INTR_FLAG_DEFAULT 0
/*
 * GPIO_OUTPUT_IO_0=18, GPIO_OUTPUT_IO_1=19
 * In binary representation,
 * 1ULL<<GPIO_OUTPUT_IO_0 is equal to 0000000000000000000001000000000000000000 and
 * 1ULL<<GPIO_OUTPUT_IO_1 is equal to 0000000000000000000010000000000000000000
 * GPIO_OUTPUT_PIN_SEL                0000000000000000000011000000000000000000
 * */
#define MODE_SETTING_PIN_LSB GPIO_NUM_18
#define MODE_SETTING_PIN_MSB GPIO_NUM_19
#define MODE_SETTING_MASK  ((1ULL<<MODE_SETTING_PIN_LSB) | (1ULL<<MODE_SETTING_PIN_MSB))

#define PRINT_STATS // Uncomment to print task stats periodically

// Tag for ESP logging
static const char* TAG_TWAI = "TWAI";
static const char* TAG_WASM = "WASM";
static const char* TAG_MCP1 = "MCP1";
static const char* TAG_MCP2 = "MCP2";
static const char* TAG_STATUS = "STATUS";

spi_device_handle_t spi1; //!< MCP controller 1 spi handle
spi_device_handle_t spi2; //!< MCP controller 2 spi handle

tNMEA2000_esp32c6 C0(MCP0_TX, MCP0_RX);   //!< Controller 0 -> TWAI, (TX_PIN, RX_PIN)
tNMEA2000_mcp C1(&spi1,MCP1_CS,MCP_8MHZ,MCP1_INT,50);      //!< Controller 1 -> MCP,  (spi_handle, CS_PIN, mcp_clk_freq, INT_PIN, _rx_frame_buf_size)
tNMEA2000_mcp C2(&spi2,MCP2_CS,MCP_8MHZ,MCP2_INT,50);      //!< Controller 2 -> MCP,  (spi_handle, CS_PIN, mcp_clk_freq, INT_PIN, _rx_frame_buf_size)



// Task Handles
static TaskHandle_t C0_send_task_handle = NULL;
static TaskHandle_t C0_receive_task_handle = NULL;
static TaskHandle_t C1_send_task_handle = NULL;
static TaskHandle_t C1_receive_task_handle = NULL;
static TaskHandle_t C2_send_task_handle = NULL;
static TaskHandle_t C2_receive_task_handle = NULL;
static TaskHandle_t stats_task_handle = NULL;
static TaskHandle_t modes_task_handle = NULL;

QueueHandle_t C0_tx_queue; //!< Queue that stores messages to be sent out on controller 0
QueueHandle_t C1_tx_queue; //!< Queue that stores messages to be sent out on controller 1
QueueHandle_t C2_tx_queue; //!< Queue that stores messages to be sent out on controller 2
QueueHandle_t rx_queue; //!< Queue that stores all messages received on all controllers
static QueueHandle_t gpio_evt_queue = NULL; //!< Queue that stores GPIO events from ISR for changing t connector mode

SemaphoreHandle_t x_sem_mcp1; //!< Semaphore handle for MCP1
SemaphoreHandle_t x_sem_mcp2; //!< Semaphore handle for MCP2

static unsigned long C0_MsgSentCount=0;
static unsigned long C0_MsgFailCount=0;
static unsigned long C1_MsgSentCount=0;
static unsigned long C1_MsgFailCount=0;
static unsigned long C2_MsgSentCount=0;
static unsigned long C2_MsgFailCount=0;

/// @brief enum to store identifiers for each controller
enum CONTROLLER {
    C0_NUM = 0,
    C1_NUM = 1,
    C2_NUM = 2
};

//----------------------------------------------------------------------------------------------------------------------------
// Forward Declarations
//----------------------------------------------------------------------------------------------------------------------------
void HandleNMEA2000Msg(const tN2kMsg &N2kMsg);
std::string nmea_to_string(NMEA_msg& msg);
void uintArrToCharrArray(uint8_t (&data_uint8_arr)[MAX_DATA_LENGTH_BTYES], unsigned char (&data_char_arr)[MAX_DATA_LENGTH_BTYES]);
//----------------------------------------------------------------------------------------------------------------------------
// Variables
//-----------------------------------------------------------------------------------------------------------------------------
char * wasm_buffer = NULL;  //!< buffer allocated for wasm app, used to hold received messages so app can access them
char * wasm_mode_buffer = NULL;  //!< buffer allocated for wasm app, used to hold current t connector mode set by Raspberry Pi
int read_msg_count = 0; //!< Used to track messages read
int send_msg_count = 0; //!< Used to track messages sent
std::string tc_mode = "1"; //!< Contains the T Connector mode-> 0 - OFF, 1 - PASSIVE, 2 - GPS_ATTACK, 3 - TBD
uint32_t alerts_to_enable = TWAI_ALERT_RX_DATA | TWAI_ALERT_TX_FAILED | TWAI_ALERT_RX_QUEUE_FULL; //!< Sets which alerts to enable for TWAI controller

// Task Counters - temporary, for debugging
int C0_rx_task_count = 0;
int C0_tx_task_count = 0;
int C1_rx_task_count = 0;
int C1_tx_task_count = 0;
int C2_rx_task_count = 0;
int C2_tx_task_count = 0;
int wasm_pthread_count = 0;
int stats_task_count = 0;
double wasm_main_duration;
//-------------------------------------------------------------------------------------------------------------------------------
// Native Functions to Export to WASM App
//-----------------------------------------------------------------------------------------------------------------------------

/**
 * @brief prints a uint8_t array to terminal
 * 
 * Native function to be exported to WASM app. Used for debugging purposes.
 * 
 * @param exec_env
 * @param input pointer to array
 * @param length length of the array
*/
void PrintStr(wasm_exec_env_t exec_env,uint8_t* input, int32_t length){
    printf("PrintStr: ");
    for (int32_t i = 0; i < length;i++ ) {
        printf("%u ", *(input+i));
    }
    printf("\n");
    return;
}

/**
 * @brief prints a integer to terminal
 * 
 * Native function to be exported to WASM app. Used for debugging purposes.
 * 
 * @param exec_env
 * @param number integer to print
 * @param hex prints the number in hex format if hex is 1, else prints it in decimal format
*/
void PrintInt32(wasm_exec_env_t exec_env,int32_t number,int32_t hex){
    if (hex == 1){
        printf("PrintInt32: %lx \n", number);
    }
    else {
        printf("PrintInt32: %li \n", number);
    }    
    return;
}

/****************************************************************************
 * \brief Puts a message in a controller send queue
 * 
 * This function is exported to the WASM app to be called from app to send a message. 
 * Creates a NMEA_msg object and puts it into the appropriate send queue. 
 * 
 * @param exec_env
 * @param[in] controller_number 
 * @param[in] priority
 * @param[in] PGN
 * @param[in] source
 * @param[in] data
 * @param[in] data_length_bytes 
 * 
 * \return 1 if message converted successfully, 0 if not.
*/
int32_t SendMsg(wasm_exec_env_t exec_env, int32_t controller_number, int32_t priority, int32_t PGN, int32_t source, uint8_t* data, int32_t data_length_bytes ){
    ESP_LOGD(TAG_WASM, "SendMsg called \n");
    NMEA_msg msg;
    msg.controller_number = controller_number;
    msg.priority = priority;
    msg.PGN = PGN;
    msg.source = source;
    msg.data_length_bytes = data_length_bytes;

    // Copy the data bytes
    for (size_t i = 0; i < data_length_bytes; ++i) {
        uint8_t value = static_cast<uint8_t>(data[i]);
        msg.data[i] = value;
    }

    
    if (controller_number == C0_NUM){
        // Add to controller 0 queue
        ESP_LOGD(TAG_WASM,"Added a msg to ctrl0_q with PGN %u \n", msg.PGN);
        if (xQueueSendToBack(C0_tx_queue, &msg, pdMS_TO_TICKS(10))){
            return 1;
        }
    }
    else if(controller_number == C1_NUM)
    {
        // Add to controller 1 queue
        ESP_LOGD(TAG_WASM,"Added a msg to ctrl1_q with PGN %u \n", msg.PGN);
        if (xQueueSendToBack(C1_tx_queue, &msg, pdMS_TO_TICKS(10))){
            return 1;
        }
    }
    else if(controller_number == C2_NUM)
    {
        // Add to controller 2 queue
        ESP_LOGD(TAG_WASM,"Added a msg to ctrl2_q with PGN %u \n", msg.PGN);
        if (xQueueSendToBack(C2_tx_queue, &msg, pdMS_TO_TICKS(10))){
            return 1;
        }
    }
    else{
        ESP_LOGE(TAG_WASM, "Invalid controller number: %" PRIu32 "", controller_number);
        return 0;
    }

    return 0;
}

//----------------------------------------------------------------------------------------------------------------------------------------------------------------
// Helper Functions for Conversions
//-----------------------------------------------------------------------------------------------------------------------------------------------------------------

/**
 * \brief converts a NMEA_msg to a string
 * @todo make sure that if pgn is only 4 digits in hex it still takes 5
 * @param[in] msg reference to a NMEA_msg object
 * \return std::string representing the message
*/
std::string nmea_to_string(NMEA_msg& msg){
    std::stringstream ss;
    ss << std::hex << std::setw(1) << std::setfill('0') << static_cast<int>(msg.controller_number);
    ss << std::hex << std::setw(1) << std::setfill('0') << static_cast<int>(msg.priority);
    ss << std::hex << std::setw(5) << std::setfill('0') << msg.PGN;
    ss << std::hex << std::setw(1) << std::setfill('0') << static_cast<int>(msg.source);
    ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(msg.data_length_bytes);
    for (uint8_t d : msg.data){
        char hex_num[3];
        sprintf(hex_num, "%X", d);
        ss << std::setw(2) << hex_num;
    }
    const std::string s = ss.str();
    return s;
    
}

/**
 * @brief Fills a char array with values from a uint8_t array
 * 
 * @param[in] data_uint8_arr
 * @param[out] data_char_arr 
 * 
*/
void uint8ArrayToCharrArray(uint8_t (&data_uint8_arr)[MAX_DATA_LENGTH_BTYES], unsigned char (&data_char_arr)[MAX_DATA_LENGTH_BTYES]){
    for (size_t i = 0; i < MAX_DATA_LENGTH_BTYES; ++i) {
        data_char_arr[i] = static_cast<unsigned char>(data_uint8_arr[i]);
    }
}

//---------------------------------------------------------------------------------------------------------------------------------------------
/**
 * @brief Interrupt Handler for gpio that determine T Connector modes
*/
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}
/**
 * @brief Configures GPIO used to read mode settings from the Raspberry Pi
 * 
*/
void configTConnectorModes()
{
    //zero-initialize the config structure.
    gpio_config_t io_conf = {};
    //disable interrupt
    io_conf.intr_type = GPIO_INTR_ANYEDGE; // Trigger on both rising and falling edge
    //set as output mode
    io_conf.mode = GPIO_MODE_INPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = MODE_SETTING_MASK;
    //disable pull-down mode
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    //disable pull-up mode
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE ;
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    //create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(MODE_SETTING_PIN_LSB, gpio_isr_handler, (void*) MODE_SETTING_PIN_LSB);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(MODE_SETTING_PIN_MSB, gpio_isr_handler, (void*) MODE_SETTING_PIN_MSB);
}
/**
 * @brief Task to get the Controller Mode set by the Raspberry Pi
 * 
 * 00 - Off
 * 01 - Passive
 * 10 - GPS Opposite Direction Attack
 * 11 - GPS Translation Attack
 * 
*/
void get_mode_task(void *pvParameters)
{
    configTConnectorModes();
    uint32_t io_num;
    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            ESP_LOGD("MODE: ","GPIO[%" PRIu32 "] intr, val: %i\n", io_num, gpio_get_level(GPIO_NUM_18));
            ESP_LOGD("MODE: ","GPIO[%" PRIu32 "] intr, val: %i\n", io_num, gpio_get_level(GPIO_NUM_19));
            int msb = gpio_get_level(GPIO_NUM_19);
            int lsb = gpio_get_level(GPIO_NUM_18);
            int mode = (msb << 1) | lsb;
            tc_mode = std::to_string(mode);
            ESP_LOGD("MODE: ", "%i",mode); 
        }
    }
}
/**
 * \brief Sends a message
 * 
 * Converts NMEA-msg to the NMEA2000 Library N2kMsg format and sends the message
 * 
 * @todo update time to take time from message
 * @todo update for multiple controllers
 * 
*/
bool SendN2kMsg(NMEA_msg msg, int controller_num) {
  tN2kMsg N2kMsg;
  N2kMsg.Priority = msg.priority;
  N2kMsg.PGN = msg.PGN;
  N2kMsg.Source = msg.source;
  N2kMsg.Destination = 0xff; //not used

  N2kMsg.DataLen = msg.data_length_bytes;

  uint8ArrayToCharrArray(msg.data, N2kMsg.Data);

  N2kMsg.MsgTime = N2kMillis64();//TODO 

  if(controller_num == C0_NUM){
    if ( C0.SendMsg(N2kMsg) ) {
      ESP_LOGD(TAG_TWAI, "sent a message \n");
      C0_MsgSentCount++;
      send_msg_count++;
    } else {
      ESP_LOGW(TAG_TWAI, "failed to send a message \n");
      C0_MsgFailCount++;
    }
  }
  else if(controller_num == C1_NUM){
    if ( C1.SendMsg(N2kMsg) ) {
      ESP_LOGD(TAG_MCP1, "sent a message \n");
      C1_MsgSentCount++;
      send_msg_count++;
    } else {
      ESP_LOGW(TAG_MCP1, "failed to send a message \n");
      C1_MsgFailCount++;
    }
  }
  else if(controller_num == C2_NUM){
    if ( C2.SendMsg(N2kMsg) ) {
      ESP_LOGD(TAG_MCP2, "sent a message \n");
      C2_MsgSentCount++;
      send_msg_count++;
    } else {
      //ESP_LOGW(TAG_MCP2, "failed to send a message \n");
      C2_MsgFailCount++;
    }
  }

    return true;
}

/**
 * @brief Prints runtime and percentage for tasks and pthreads
 * 
 * To use this function, you need to configure some settings in menuconfig. 
 * 
 * You must enable FreeRTOS to collect runtime stats under 
 * Component Config -> FreeRTOS -> Kernel -> configGENERATE_RUN_TIME_STATS
 * 
 * You must also choose the clock source for run time stats configured under
 * Component Config -> FreeRTOS -> Port -> Choose the clock source for runtime stats.
 * The esp_timer should be selected by default. 
 * This option will affect the time unit resolution in which the statistics
 *  are measured with respect to.
 * 
 * 
 * 
 * @param[in] xTicksToWait
 * 
*/
static esp_err_t print_real_time_stats(TickType_t xTicksToWait)
{
    TaskStatus_t *start_array = NULL, *end_array = NULL;
    UBaseType_t start_array_size, end_array_size;
    uint32_t start_run_time, end_run_time;
    esp_err_t ret;

    //Allocate array to store current task states
    start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    start_array = reinterpret_cast<TaskStatus_t*>(std::malloc(sizeof(TaskStatus_t) * start_array_size));
    if (start_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        free(start_array);
        free(end_array);
        return ret;
    }
    //Get current task states
    start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
    if (start_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        free(start_array);
        free(end_array);
        return ret;
    }

    vTaskDelay(xTicksToWait);

    //Allocate array to store tasks states post delay
    end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    end_array = reinterpret_cast<TaskStatus_t*>(std::malloc(sizeof(TaskStatus_t) * end_array_size));
    if (end_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        free(start_array);
        free(end_array);
        return ret;
    }
    //Get post delay task states
    end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
    if (end_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        free(start_array);
        free(end_array);
        return ret;
    }

    //Calculate total_elapsed_time in units of run time stats clock period.
    uint32_t total_elapsed_time = (end_run_time - start_run_time);
    if (total_elapsed_time == 0) {
        ret = ESP_ERR_INVALID_STATE;
        free(start_array);
        free(end_array);
        return ret;
    }

    printf("| Task | Run Time | Percentage\n");
    //Match each task in start_array to those in the end_array
    for (int i = 0; i < start_array_size; i++) {
        int k = -1;
        for (int j = 0; j < end_array_size; j++) {
            if (start_array[i].xHandle == end_array[j].xHandle) {
                k = j;
                //Mark that task have been matched by overwriting their handles
                start_array[i].xHandle = NULL;
                end_array[j].xHandle = NULL;
                break;
            }
        }
        //Check if matching task found
        if (k >= 0) {
            uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
            uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * portNUM_PROCESSORS);
            printf("| %s | %" PRIu32 " | %" PRIu32 "%%\n", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
        }
    }

    //Print unmatched tasks
    for (int i = 0; i < start_array_size; i++) {
        if (start_array[i].xHandle != NULL) {
            printf("| %s | Deleted\n", start_array[i].pcTaskName);
        }
    }
    for (int i = 0; i < end_array_size; i++) {
        if (end_array[i].xHandle != NULL) {
            printf("| %s | Created\n", end_array[i].pcTaskName);
        }
    }
    ret = ESP_OK;
    free(start_array);
    free(end_array);
    return ret;

}


/**
 * @brief Retrieves twai status and alerts
 * 
 * Used to display information regarding queue's filling up, and how many messages have been sent/received.
 * 
 * @param[in] TAG
*/
void GetStatus(const char* TAG){
    uint32_t alerts = 0;
    C0.ReadAlerts(alerts, pdMS_TO_TICKS(1));
    if (alerts & TWAI_ALERT_RX_QUEUE_FULL){
        ESP_LOGW(TAG, "TWAI rx queue full");
    } 
    twai_status_info_t status;
    C0.GetTwaiStatus(status);
    ESP_LOGI(TAG, "TWAI Msgs queued for transmission: %" PRIu32 " Unread messages in rx queue: %" PRIu32, status.msgs_to_tx, status.msgs_to_rx);
    ESP_LOGI(TAG, "Msgs lost due to TWAI RX FIFO overrun: %" PRIu32 "", status.rx_overrun_count);
    ESP_LOGI(TAG, "Msgs lost due to full TWAI RX queue: %" PRIu32 "", status.rx_missed_count);
    ESP_LOGI(TAG, "Messages Read: %d, Messages Sent %d", read_msg_count, send_msg_count);
    UBaseType_t msgs_in_rx_q = uxQueueMessagesWaiting(rx_queue);
    ESP_LOGI(TAG, "Received Messages queue size: %d \n", msgs_in_rx_q);
    UBaseType_t msgs_in_q0 = uxQueueMessagesWaiting(C0_tx_queue);
    UBaseType_t msgs_in_q1 = uxQueueMessagesWaiting(C1_tx_queue);
    UBaseType_t msgs_in_q2 = uxQueueMessagesWaiting(C2_tx_queue);
    ESP_LOGI(TAG, "Controller 0 send queue size: %d \n", msgs_in_q0);
    ESP_LOGI(TAG, "Controller 1 send queue size: %d \n", msgs_in_q1);
    ESP_LOGI(TAG, "Controller 2 send queue size: %d \n", msgs_in_q2);

    // Task Counters
    ESP_LOGI(TAG, "RX task count: %d", C0_rx_task_count);
    ESP_LOGI(TAG, "TX task count: %d", C0_tx_task_count);
    ESP_LOGI(TAG, "MCP1 RX task count: %d", C1_rx_task_count);
    ESP_LOGI(TAG, "MCP1 TX task count: %d", C1_tx_task_count);
    ESP_LOGI(TAG, "MCP2 RX task count: %d", C2_rx_task_count);
    ESP_LOGI(TAG, "MCP2 TX task count: %d", C2_tx_task_count);
    ESP_LOGI(TAG, "Wasm pthread count: %d", wasm_pthread_count);
    ESP_LOGI(TAG, "Stats task count: %d", stats_task_count);

    //Duration of the app_instance_main for the wasm pthread
    ESP_LOGI(TAG, "Duration of wasm task (ms): %f",wasm_main_duration/1000000);
}

/**
 * @brief Optional FreeRTOS task for printing status messages for debugging 
 * 
 * @param pvParameters
 * 
*/
static void stats_task(void *arg)
{
    //Print real time stats periodically
    while (1) {
        printf("\n\nGetting real time stats over %" PRIu32 " ticks\n", STATS_TICKS);
        if (print_real_time_stats(STATS_TICKS) == ESP_OK) {
            printf("Real time stats obtained\n");
        } else {
            printf("Error getting real time stats\n");
        }
        GetStatus(TAG_STATUS);
        stats_task_count++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

//--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Controller 0 (TWAI)
//--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
/**
 * @brief FreeRTOS task for receiving messages from CAN controller
 * 
 * @param pvParameters
 * 
 * NMEA2000 Library is designed so that message receiving and sending is handled within the same task. 
 * In the NMEA2000_ESP32 library, this is made possible by letting the twai rx interrupt handle receiving CAN frames. 
 * In this library, the receiving is separated from the processing and sending of messages. This is done because 
 * I was unable to trigger recieving a CAN frame from the twai rx interrupt, so CAN_read_frame() must be called explicitly here. 
*/
void C0_receive_task(void *pvParameters){
    esp_log_level_set(TAG_TWAI, MY_ESP_LOG_LEVEL);
    C0.SetN2kCANMsgBufSize(8);
    C0.SetN2kCANReceiveFrameBufSize(250);
    C0.EnableForward(false);               
    C0.SetMsgHandler(HandleNMEA2000Msg);
    C0.SetMode(tNMEA2000::N2km_ListenAndSend);    
    C0.Open();
    C0.ConfigureAlerts(alerts_to_enable);

    // Task Loop
    while(1)
    {
        C0.CAN_read_frame(); // retrieves available messages - for TWAI controller only
        C0.ParseMessages(); // Calls message handle whenever a message is available
        C0_rx_task_count++;        
    }
    vTaskDelete(NULL); // should never get here...
}

/**
 * @brief FreeRTOS task for processing and sending messages from CAN controller with NMEA2000 library
 * 
 * Tries to receive a message from the controller 0 tx queue, and sends it if available
 * 
 * @todo frame buffer should be 32 - see if this works
 * @param pvParameters
*/
void C0_send_task(void *pvParameters)
{   
    esp_log_level_set(TAG_TWAI, MY_ESP_LOG_LEVEL);
    ESP_LOGI(TAG_TWAI, "Starting C0_send_task");
    NMEA_msg msg;

    // Task Loop
    for (;;)
    {
        if( xQueueReceive( C0_tx_queue, &msg, (100 / portTICK_PERIOD_MS) ))
        {

            SendN2kMsg(msg, C0_NUM);
  
        }
        ESP_LOGV(TAG_TWAI, "Send task called");

        C0_tx_task_count++;        
    }
    vTaskDelete(NULL); // should never get here...
}

//--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Controller 1 (MCP)
//--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
/**
 * @brief FreeRTOS task for receiving messages from MCP CAN controller 1
 * 
 * Sets up a NMEA2000 Object with the MCP class. Initializes the SPI bus and adds a device to the bus. 
 * Semaphore is used so that send and receive tasks don't access the same device at the same time. 
 * 
 * @param pvParameters
 * 
*/
void C1_receive_task(void *pvParameters){
    esp_log_level_set(TAG_MCP1, MY_ESP_LOG_LEVEL);
    C1.SetN2kCANMsgBufSize(8);
    C1.SetN2kCANReceiveFrameBufSize(250);
    C1.EnableForward(false);              
    C1.SetMsgHandler(HandleNMEA2000Msg);
    C1.SetMode(tNMEA2000::N2km_ListenAndSend);
    C1.CANinit(); // Initialize SPI bus, call before C1.Open() and only call once for all the MCP tasks
    C1.Open(); 

    // Task Loop
    while(1)
    {
        if( xSemaphoreTake( x_sem_mcp1, (100 / portTICK_PERIOD_MS) ) == pdTRUE )
        {
            // We were able to obtain the semaphore and can now access the shared resource.
            C1.ParseMessages(); // Calls message handle whenever a message is available    
            // We have finished accessing the shared resource.  Release the semaphore.
            xSemaphoreGive( x_sem_mcp1 );
            vTaskDelay(10 / portTICK_PERIOD_MS);   
        }
        C1_rx_task_count++;
        vTaskDelay(10 / portTICK_PERIOD_MS);        
    
    }
    vTaskDelete(NULL); // should never get here...
}

/**
 * @brief FreeRTOS task for processing and sending messages from MCP CAN controller 1 with NMEA2000 library
 * 
 * Tries to receive a message from the controller 1 tx queue, and sends it if available
 * Semaphore is used so that send and receive tasks don't access the same device at the same time. 
 * 
 * @param pvParameters
*/
void C1_send_task(void *pvParameters)
{   
    esp_log_level_set(TAG_MCP1, MY_ESP_LOG_LEVEL);
    ESP_LOGI(TAG_TWAI, "Starting C1_send_task");
    NMEA_msg msg;

    // Task Loop
    for (;;)
    {
        if( xQueueReceive( C1_tx_queue, &msg, (100 / portTICK_PERIOD_MS) ))
        {
            if( xSemaphoreTake( x_sem_mcp1, portMAX_DELAY ) == pdTRUE )
            {
                // We were able to obtain the semaphore and can now access the shared resource.
                ESP_LOGD(TAG_MCP1, "About to send message with PGN: %i", msg.PGN);

                SendN2kMsg(msg, C1_NUM);
                
                // We have finished accessing the shared resource.  Release the semaphore.
                xSemaphoreGive( x_sem_mcp1 );
            }        
            
        }
        ESP_LOGD(TAG_TWAI, "Send task called");

        C1_tx_task_count++;        
    }
    vTaskDelete(NULL); // should never get here...
}

//--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Controller 2 (MCP)
//--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
/**
 * @brief FreeRTOS task for receiving messages from MCP CAN controller 2
 * 
 * Sets up a NMEA2000 Object with the MCP class. Adds a device to the bus. It does not need to initialize the SPI bus because the MCP1 recieve task has already done that.
 * Semaphore is used so that send and receive tasks don't access the same device at the same time. 
 * 
 * @param pvParameters
 * 
*/
void C2_receive_task(void *pvParameters){
    esp_log_level_set(TAG_MCP2, MY_ESP_LOG_LEVEL);

    C2.SetN2kCANMsgBufSize(8);
    C2.SetN2kCANReceiveFrameBufSize(250);
    C2.EnableForward(false);              
    C2.SetMsgHandler(HandleNMEA2000Msg);
    C2.SetMode(tNMEA2000::N2km_ListenAndSend);

    C2.Open();
    // Task Loop
    while(1)
    {
        if( xSemaphoreTake( x_sem_mcp2, (100 / portTICK_PERIOD_MS) ) == pdTRUE )
        {
            // We were able to obtain the semaphore and can now access the shared resource.
            C2.ParseMessages(); // Calls message handle whenever a message is available     

            xSemaphoreGive( x_sem_mcp2 ); // We have finished accessing the shared resource.  Release the semaphore.
            vTaskDelay(100 / portTICK_PERIOD_MS);   
        }
        C2_rx_task_count++;
        vTaskDelay(10 / portTICK_PERIOD_MS);           
    
    }
    vTaskDelete(NULL); // should never get here...
}

/**
 * @brief FreeRTOS task for processing and sending messages from MCP CAN controller 2 with NMEA2000 library
 * 
 * Tries to receive a message from the controller 2 tx queue, and sends it if available
 * Semaphore is used so that send and receive tasks don't access the same device at the same time. 
 * 
 * @param pvParameters
*/
void C2_send_task(void *pvParameters)
{   
    esp_log_level_set(TAG_MCP2, MY_ESP_LOG_LEVEL);
    ESP_LOGI(TAG_TWAI, "Starting C2_send_task");
    NMEA_msg msg;
    
    // Task Loop
    for (;;)
    {
        if( xQueueReceive( C2_tx_queue, &msg, (100 / portTICK_PERIOD_MS) ))
        {
            if( xSemaphoreTake( x_sem_mcp2, portMAX_DELAY ) == pdTRUE )
            {
                // We were able to obtain the semaphore and can now access the shared resource.
                ESP_LOGD(TAG_MCP2, "About to send message with PGN: %i", msg.PGN);

                SendN2kMsg(msg, C2_NUM);            
                xSemaphoreGive( x_sem_mcp2 ); // We have finished accessing the shared resource.  Release the semaphore.
            }        
            
        }
        ESP_LOGD(TAG_TWAI, "Send task called");

        C2_tx_task_count++;        
    }
    vTaskDelete(NULL); // should never get here...
}

/**
 * \brief Creates a NMEA_msg object and adds it to.data the received messages queue
 * 
 * @todo handle out of range data
 * \param N2kMsg Reference to the N2KMs being handled
 * \return void
 */
void HandleNMEA2000Msg(const tN2kMsg &N2kMsg) {
  if (N2kMsg.Source == 14){
    ESP_LOGD(TAG_TWAI, "source is 14");
    return;
  }
  ESP_LOGV(TAG_TWAI, "Message Handler called");
  NMEA_msg msg;
  msg.controller_number = 0;
  msg.priority = N2kMsg.Priority;
  
  msg.PGN = N2kMsg.PGN;
  ESP_LOGD(TAG_TWAI, "PGN %u", msg.PGN);
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

  if(xQueueSendToBack(rx_queue, &msg, pdMS_TO_TICKS(10)) == 0){
    ESP_LOGW(TAG_TWAI, "Could not add received message to RX queue");    
  }
  else{
    ESP_LOGV(TAG_TWAI, " added msg to received queue");
  }
  read_msg_count++;
  
}



/**
 * @brief executes main function in wasm app
 * 
 * @param module_inst wasm module instance
*/
static void * app_instance_main(wasm_module_inst_t module_inst)
{
    const char *exception;

    wasm_application_execute_main(module_inst, 0, NULL);
    if ((exception = wasm_runtime_get_exception(module_inst)))
        ESP_LOGW(TAG_WASM,"%s\n", exception);
    return NULL;
}

/**
 * @brief WASM pthread to host wasm app
 * 
 * Sets up the wasm environment. 
 * Links native function to be exported. 
 * Calls wasm app function to link allocated wasm buffer.
 * Runs main function in wasm app.
 * 
 * @param arg unused - I don't know why this is required
*/
void * iwasm_main(void *arg)
{
    esp_log_level_set(TAG_WASM, MY_ESP_LOG_LEVEL);
    (void)arg; /* unused */
    /* setup variables for instantiating and running the wasm module */
    uint8_t *wasm_file_buf = NULL;
    unsigned wasm_file_buf_size = 0;
    wasm_exec_env_t exec_env = NULL;
    wasm_module_t wasm_module = NULL;
    wasm_module_inst_t wasm_module_inst = NULL;
    char error_buf[128];
    void *ret;
    wasm_function_inst_t func = NULL;
    RuntimeInitArgs init_args;

    uint32_t buffer_for_wasm = 0;
    uint32_t buffer_for_wasm_mode = 0;

    /* configure memory allocation */
    memset(&init_args, 0, sizeof(RuntimeInitArgs));

    /* the native functions that will be exported to WASM app */
    static NativeSymbol native_symbols[] = {
        {
            "PrintStr", // the name of WASM function name
            reinterpret_cast<void*>(PrintStr),    // the native function pointer
            "($i)",  // the function prototype signature, avoid to use i32
            NULL        // attachment is NULL
        },
        {
            "PrintInt32",
            reinterpret_cast<void*>(PrintInt32),   
            "(ii)", 
            NULL      
        },
        {
            "SendMsg",
            reinterpret_cast<void*>(SendMsg),   
            "(iiii*~)i",
            NULL    
        }
    };
#if WASM_ENABLE_GLOBAL_HEAP_POOL == 0
    init_args.mem_alloc_type = Alloc_With_Allocator;
    init_args.mem_alloc_option.allocator.malloc_func = (void *)os_malloc;
    init_args.mem_alloc_option.allocator.realloc_func = (void *)os_realloc;
    init_args.mem_alloc_option.allocator.free_func = (void *)os_free;
#else
#error The usage of a global heap pool is not implemented yet for esp-idf.
#endif

    /* configure the native functions being exported to WASM app */
    init_args.n_native_symbols = sizeof(native_symbols) / sizeof(NativeSymbol);
    init_args.native_module_name = "env";
    init_args.native_symbols = native_symbols;


    ESP_LOGI(TAG_WASM, "Initialize WASM runtime");
    /* initialize runtime environment */
    if (!wasm_runtime_full_init(&init_args)) {
        ESP_LOGE(TAG_WASM, "Init runtime failed.");
        return NULL;
    }
    ESP_LOGI(TAG_WASM, "Run wamr with interpreter");

    wasm_file_buf = (uint8_t *)nmea_attack_wasm;
    wasm_file_buf_size = sizeof(nmea_attack_wasm);

    /* load WASM module */
    if (!(wasm_module = wasm_runtime_load(wasm_file_buf, wasm_file_buf_size,
                                          error_buf, sizeof(error_buf)))) {
        ESP_LOGE(TAG_WASM, "Error in wasm_runtime_load: %s", error_buf);
        goto fail;
    }

    ESP_LOGI(TAG_WASM, "Instantiate WASM runtime");
    if (!(wasm_module_inst =
              wasm_runtime_instantiate(wasm_module, NATIVE_STACK_SIZE, // stack size
                                       NATIVE_HEAP_SIZE,              // heap size
                                       error_buf, sizeof(error_buf)))) {
        ESP_LOGE(TAG_WASM, "Error while instantiating: %s", error_buf);
        goto fail;
    }

    
    exec_env = wasm_runtime_create_exec_env(wasm_module_inst, NATIVE_STACK_SIZE);//stack size
    if (!exec_env) {
        ESP_LOGW(TAG_WASM,"Create wasm execution environment failed.\n");
        goto fail;
    }

    // Link buffer for Messages
    ESP_LOGI(TAG_WASM, "Malloc buffer in wasm function");
    buffer_for_wasm = wasm_runtime_module_malloc(wasm_module_inst, 100, (void **)&wasm_buffer);
    if (buffer_for_wasm == 0) {
        ESP_LOGI(TAG_WASM, "Malloc failed");
        goto fail;
    }
    uint32 argv[2];
    argv[0] = buffer_for_wasm;     /* pass the buffer address for WASM space */
    argv[1] = MSG_BUFFER_SIZE;                 /* the size of buffer */
    ESP_LOGI(TAG_WASM, "Call wasm function");
    /* it is runtime embedder's responsibility to release the memory,
       unless the WASM app will free the passed pointer in its code */
    
    if (!(func = wasm_runtime_lookup_function(wasm_module_inst, "link_msg_buffer",
                                               NULL))) {
        ESP_LOGW(TAG_WASM,
            "The wasm function link_msg_buffer wasm function is not found.\n");
        goto fail;
    }

    if (wasm_runtime_call_wasm(exec_env, func, 2, argv)) {
        ESP_LOGI(TAG_WASM,"Native finished calling wasm function: link_msg_buffer, "
               "returned a formatted string: %s\n",
               wasm_buffer);
    }
    else {
        ESP_LOGW(TAG_WASM,"call wasm function link_msg_buffer failed. error: %s\n",
               wasm_runtime_get_exception(wasm_module_inst));
        goto fail;
    }

    // Link buffer for mode
    ESP_LOGI(TAG_WASM, "Malloc buffer in wasm function");
    buffer_for_wasm_mode = wasm_runtime_module_malloc(wasm_module_inst, 100, (void **)&wasm_mode_buffer);
    if (buffer_for_wasm_mode == 0) {
        ESP_LOGI(TAG_WASM, "Malloc failed");
        goto fail;
    }
    uint32 argv_mode[2];
    argv_mode[0] = buffer_for_wasm_mode;     /* pass the buffer address for WASM space */
    argv_mode[1] = MODE_BUFFER_SIZE;                 /* the size of buffer */
    ESP_LOGI(TAG_WASM, "Call wasm function");
    /* it is runtime embedder's responsibility to release the memory,
       unless the WASM app will free the passed pointer in its code */
    
    if (!(func = wasm_runtime_lookup_function(wasm_module_inst, "link_mode_buffer",
                                               NULL))) {
        ESP_LOGW(TAG_WASM,
            "The wasm function link_mode_buffer wasm function is not found.\n");
        goto fail;
    }

    if (wasm_runtime_call_wasm(exec_env, func, 2, argv_mode)) {
        ESP_LOGI(TAG_WASM,"Native finished calling wasm function: link_mode_buffer, "
               "returned a formatted string: %s\n",
               wasm_mode_buffer);
    }
    else {
        ESP_LOGW(TAG_WASM,"call wasm function link_mode_buffer failed. error: %s\n",
               wasm_runtime_get_exception(wasm_module_inst));
        goto fail;
    }


    // Task Loop
    while (true){
        ESP_LOGV(TAG_WASM, "run main() of the application");
        auto start = std::chrono::high_resolution_clock::now(); 
        NMEA_msg msg;
        if (xQueueReceive(rx_queue, &msg, (100 / portTICK_PERIOD_MS) == 1)){
            std::string str_msg = nmea_to_string(msg);
            strncpy(wasm_buffer, str_msg.c_str(), str_msg.size()); // fill message buffer
            strncpy(wasm_mode_buffer, tc_mode.c_str(), tc_mode.size()); // fill mode buffer            
            ret = app_instance_main(wasm_module_inst);  //Call the main function 
            assert(!ret);
        } else{
            vTaskDelay(10 / portTICK_PERIOD_MS); // I don't understand why this is nessesary
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto ns_duration = duration_cast<std::chrono::nanoseconds>(end-start);
        wasm_main_duration = static_cast<double>(ns_duration.count());
        
        wasm_pthread_count++;
    }


    //wasm_runtime_module_free(wasm_module_inst, buffer_for_wasm);
    //wasm_runtime_module_free(wasm_module_inst, buffer_for_wasm_mode);
    
    /* destroy the module instance */
    ESP_LOGI(TAG_WASM, "Deinstantiate WASM runtime");
    wasm_runtime_deinstantiate(wasm_module_inst);

fail:
    if (exec_env)
        wasm_runtime_destroy_exec_env(exec_env);
    if (wasm_module_inst) {
        if (buffer_for_wasm){
            wasm_runtime_module_free(wasm_module_inst, buffer_for_wasm);}
        if (buffer_for_wasm_mode)
        {
           wasm_runtime_module_free(wasm_module_inst, buffer_for_wasm_mode); 
        }
        wasm_runtime_deinstantiate(wasm_module_inst);
    }
    if (wasm_module){
        /* unload the module */
        ESP_LOGI(TAG_WASM, "Unload WASM module");
        wasm_runtime_unload(wasm_module);
    }
    if (wasm_buffer)
        BH_FREE(wasm_buffer);
    if (wasm_mode_buffer)
        BH_FREE(wasm_mode_buffer);


    /* destroy runtime environment */
    ESP_LOGI(TAG_WASM, "Destroy WASM runtime");
    wasm_runtime_destroy();
    return NULL;
}

/**
 * @brief Creates a FreeRTOS task for sending and receiving to and from CAN Controller and creates a pthread to run WASM app
 * 
 * In ESP-IDF, a pthread is just a wrapper on FreeRTOS
*/
extern "C" int app_main(void)
{
    C0_tx_queue = xQueueCreate(TX_QUEUE_SIZE, sizeof(NMEA_msg));
    C1_tx_queue = xQueueCreate(TX_QUEUE_SIZE, sizeof(NMEA_msg));
    C2_tx_queue = xQueueCreate(TX_QUEUE_SIZE, sizeof(NMEA_msg));
    rx_queue = xQueueCreate(RX_QUEUE_SIZE, sizeof(NMEA_msg));

    x_sem_mcp1 = xSemaphoreCreateMutex();
    x_sem_mcp2 = xSemaphoreCreateMutex();

    esp_err_t result = ESP_OK;

#ifdef PRINT_STATS
    /* Status Task*/
    printf( "create task");
    xTaskCreatePinnedToCore(
        &stats_task,            // Pointer to the task entry function.
        "stats_task",           // A descriptive name for the task for debugging.
        4096,                 // size of the task stack in bytes.
        NULL,                 // Optional pointer to pvParameters
        STATS_TASK_PRIO, // priority at which the task should run
        &stats_task_handle,      // Optional pass back task handle
        1
    );
    if (stats_task_handle == NULL)
    {
        printf("Unable to create task.");
        result = ESP_ERR_NO_MEM;
        goto err_out;
    }
#endif
    /* T Connector Modes Task*/
    printf( "create task");
    xTaskCreatePinnedToCore(
        &get_mode_task,            // Pointer to the task entry function.
        "get_mode_task",           // A descriptive name for the task for debugging.
        4096,                 // size of the task stack in bytes.
        NULL,                 // Optional pointer to pvParameters
        STATS_TASK_PRIO, // priority at which the task should run
        &modes_task_handle,      // Optional pass back task handle
        1
    );
    if (modes_task_handle == NULL)
    {
        printf("Unable to create task.");
        result = ESP_ERR_NO_MEM;
        goto err_out;
    }
    /* Controller 0 Sending task */
    ESP_LOGV(TAG_WASM, "create task");
    xTaskCreatePinnedToCore(
        &C0_send_task,            // Pointer to the task entry function.
        "Send_task",           // A descriptive name for the task for debugging.
        3072,                 // size of the task stack in bytes.
        NULL,                 // Optional pointer to pvParameters
        tskIDLE_PRIORITY+1, // priority at which the task should run
        &C0_send_task_handle,      // Optional pass back task handle
        1
    );
    if (C0_send_task_handle == NULL)
    {
        ESP_LOGE(TAG_TWAI, "Unable to create task.");
        result = ESP_ERR_NO_MEM;
        goto err_out;
    }
    /* Controller 0 Receiving task */
    ESP_LOGV(TAG_TWAI, "create task");
    xTaskCreatePinnedToCore(
        &C0_receive_task,            // Pointer to the task entry function.
        "Receive_task",           // A descriptive name for the task for debugging.
        3072,                 // size of the task stack in bytes.
        NULL,                 // Optional pointer to pvParameters
        tskIDLE_PRIORITY+3, // priority at which the task should run
        &C0_receive_task_handle,      // Optional pass back task handle
        0
    );
    if (C0_receive_task_handle == NULL)
    {
        ESP_LOGE(TAG_TWAI, "Unable to create task.");
        result = ESP_ERR_NO_MEM;
        goto err_out;

    }

    /* Controller 1 Sending task */
    ESP_LOGV(TAG_MCP1, "create task");
    xTaskCreatePinnedToCore(
        &C1_send_task,            // Pointer to the task entry function.
        "Send_task",           // A descriptive name for the task for debugging.
        3072,                 // size of the task stack in bytes.
        NULL,                 // Optional pointer to pvParameters
        tskIDLE_PRIORITY+1, // priority at which the task should run
        &C1_send_task_handle,      // Optional pass back task handle
        1
    );
    if (C1_send_task_handle == NULL)
    {
        ESP_LOGE(TAG_MCP1, "Unable to create task.");
        result = ESP_ERR_NO_MEM;
        goto err_out;
    }
    ///* Controller 1 Receiving task */
    ESP_LOGV(TAG_MCP1, "create task");
    xTaskCreatePinnedToCore(
        &C1_receive_task,            // Pointer to the task entry function.
        "Receive_task",           // A descriptive name for the task for debugging.
        3072,                 // size of the task stack in bytes.
        NULL,                 // Optional pointer to pvParameters
        tskIDLE_PRIORITY+3, // priority at which the task should run
        &C1_receive_task_handle,      // Optional pass back task handle
        0
    );
    if (C1_receive_task_handle == NULL)
    {
        ESP_LOGE(TAG_MCP1, "Unable to create task.");
        result = ESP_ERR_NO_MEM;
        goto err_out;
    }
    /* Controller 2 Sending task */
    ESP_LOGV(TAG_MCP2, "create task");
    xTaskCreatePinnedToCore(
        &C2_send_task,            // Pointer to the task entry function.
        "Send_task",           // A descriptive name for the task for debugging.
        3072,                 // size of the task stack in bytes.
        NULL,                 // Optional pointer to pvParameters
        tskIDLE_PRIORITY+1, // priority at which the task should run
        &C2_send_task_handle,      // Optional pass back task handle
        1
    );
    if (C2_send_task_handle == NULL)
    {
        ESP_LOGE(TAG_MCP2, "Unable to create task.");
        result = ESP_ERR_NO_MEM;
        goto err_out;
    }
    ///* Controller 2 Receiving task */
    ESP_LOGV(TAG_MCP2, "create task");
    xTaskCreatePinnedToCore(
        &C2_receive_task,            // Pointer to the task entry function.
        "Receive_task",           // A descriptive name for the task for debugging.
        3072,                 // size of the task stack in bytes.
        NULL,                 // Optional pointer to pvParameters
        tskIDLE_PRIORITY+3, // priority at which the task should run
        &C2_receive_task_handle,      // Optional pass back task handle
        0
    );
    if (C2_receive_task_handle == NULL)
    {
        ESP_LOGE(TAG_MCP2, "Unable to create task.");
        result = ESP_ERR_NO_MEM;
        goto err_out;
    }

    /* Wasm pthread */
    pthread_t t;
    int res;
    esp_pthread_cfg_t esp_pthread_cfg;



    pthread_attr_t tattr;
    pthread_attr_init(&tattr);
    pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_JOINABLE);
    pthread_attr_setstacksize(&tattr, PTHREAD_STACK_SIZE);

    // Use the ESP-IDF API to change the default thread attributes
    esp_pthread_cfg = esp_pthread_get_default_config();
    ESP_LOGI(TAG_WASM, "Pthread priority: %d", esp_pthread_cfg.prio);
    ESP_LOGI(TAG_WASM, "Pthread core: %d", esp_pthread_cfg.pin_to_core);
    esp_pthread_cfg.prio = tskIDLE_PRIORITY+1; //change priority 
    esp_pthread_cfg.pin_to_core = 1; // pin to core 1
    ESP_ERROR_CHECK( esp_pthread_set_cfg(&esp_pthread_cfg) );

    res = pthread_create(&t, &tattr, iwasm_main, (void *)NULL);
    assert(res == 0);

    esp_pthread_get_cfg(&esp_pthread_cfg);
    ESP_LOGI(TAG_WASM, "Pthread priority: %d", esp_pthread_cfg.prio);
    res = pthread_join(t, NULL);
    assert(res == 0);

err_out:
    if (result != ESP_OK)
    {
        if (C0_send_task_handle != NULL || C0_receive_task_handle != NULL || C1_send_task_handle != NULL || C1_receive_task_handle != NULL || C2_send_task_handle != NULL || C2_receive_task_handle != NULL|| stats_task_handle != NULL)
        {
            vTaskDelete(C0_send_task_handle);
            vTaskDelete(C0_receive_task_handle);
            vTaskDelete(C1_send_task_handle);
            vTaskDelete(C1_receive_task_handle);
            vTaskDelete(C2_send_task_handle);
            vTaskDelete(C2_receive_task_handle);
            vTaskDelete(stats_task_handle);
            C0_send_task_handle = NULL;
            C0_receive_task_handle = NULL;
            C1_send_task_handle = NULL;
            C1_receive_task_handle = NULL;
            C2_send_task_handle = NULL;
            C2_receive_task_handle = NULL;
            stats_task_handle = NULL;
        }
    }

    return 0;
};