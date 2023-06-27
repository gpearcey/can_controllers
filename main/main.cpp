/**
 * @file main.cpp
 * 
 * @brief Contains FreeRTOS tasks and pthread for sending and receiving messages between WASM app and CAN controller.
 * @todo convert relavent ESP_LOGI to ESP_LOGD
*/

/** 
 * @mainpage Can Controllers Documentation
 * @section intro_sec Introduction
 * This is the Triangle C++ library for C++ Documentation Tutorial.
 * @section install_sec Installation
 *
 * @subsection install_dependencies Installing Dependencies
 * Do somethings ...
 * @subsection install_library Installing Library
 * Do somethings ...
 * @subsection install_example Installing Examples
 * Do somethings ...
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "NMEA_msg.h"
#include "esp_log.h"
#include <N2kMsg.h>
#include <NMEA2000_esp32.h> 
#include <NMEA2000.h>
#include <N2kMessages.h>

#include "driver/gpio.h"
#include "bi-inc/attr_container.h"

#include "wasm_export.h"
#include "bh_read_file.h"
#include "bh_getopt.h"
#include "bh_platform.h"

#include <queue>
#include <string>
#include <iomanip>

//WebAssembley App
#include "nmea_attack.h" 

#define NATIVE_STACK_SIZE (4*1024)

// Tag for ESP logging
static const char* TAG = "main.cpp";

/**
 * @brief Creates a NMEA2000 Object set to GPIO 32 (TX), and GPIO 32 (RX)
*/
tNMEA2000_esp32 NMEA2000(GPIO_NUM_32, GPIO_NUM_34);

static TaskHandle_t N2K_task_handle = NULL;

static unsigned long N2kMsgSentCount=0;
static unsigned long N2kMsgFailCount=0;

//----------------------------------------------------------------------------------------------------------------------------
// Forward Declarations
//----------------------------------------------------------------------------------------------------------------------------
void HandleNMEA2000Msg(const tN2kMsg &N2kMsg);
std::string nmea_to_string(NMEA_msg& msg);
void vectorToCharArray(const std::vector<uint8_t>& data_vec, unsigned char (&data_char_arr)[223]);

//----------------------------------------------------------------------------------------------------------------------------
// Variables
//-----------------------------------------------------------------------------------------------------------------------------
char * wasm_buffer = NULL;  //!< buffer allocated for wasm app, used to hold received messages so app can access them
std::queue<NMEA_msg> received_msgs_q; //! Queue that stores all messages received on all controllers
std::queue<NMEA_msg> ctrl0_q; //! Queue that stores messages to be sent out on controller 0

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
 * \brief puts a message in the wasm buffer. 
 * 
 * This function is exported to the WASM app to be called from app to receive a message. 
 * Converts a NMEA_msg object into a string which is placed in the buffer.
 * @param exec_env
 * \return 1 if message converted successfully, 0 if not.
*/
int32_t GetMsg(wasm_exec_env_t exec_env){
  if (received_msgs_q.empty()){
    ESP_LOGI(TAG, "No messages available to send to app\n");
    return 0;
  }
  NMEA_msg msg = received_msgs_q.front();
  printf("about to add msg with pgn %u \n", msg.PGN);
  received_msgs_q.pop();//TODO - probably don't delete it here in case send doesn't work properly
  std::string str_msg = nmea_to_string(msg);
        printf(" string form of message: ");
  printf(str_msg.c_str());
  printf("\n");
  strncpy(wasm_buffer, str_msg.c_str(), str_msg.size());
  printf("wasm_buffer has values: %c %c %c %c %c %c %c %c %c \n",*wasm_buffer, *(wasm_buffer+1), *(wasm_buffer + 2), *(wasm_buffer+3), *(wasm_buffer+4), *(wasm_buffer+5), *(wasm_buffer+6), *(wasm_buffer+7), *(wasm_buffer+8));
  
  ESP_LOGI(TAG, "Added message to wasm app wasm_buffer");
  return 1;
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
    ESP_LOGI(TAG, "SendMsg called \n");
    NMEA_msg msg;
    msg.controller_number = controller_number;
    msg.priority = priority;
    msg.PGN = PGN;
    msg.source = source;
    msg.data_length_bytes = data_length_bytes;
    msg.data = std::vector<uint8_t>(data, data + data_length_bytes);


    for (size_t i = 0; i < data_length_bytes; ++i) {
        uint8_t value = static_cast<uint8_t>(data[i]);
        msg.data.push_back(value);
    }

    if (controller_number == 0){
        ESP_LOGI(TAG,"Added a msg to ctrl0_q with PGN %u \n", msg.PGN);
        ctrl0_q.push(msg);
        return 1;
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
        //printf("data is %i ", d);
        //std::stringstream s;
        char hex_num[3];
        sprintf(hex_num, "%X", d);
        //s << 
        ss << std::setw(2) << hex_num;
        //printf("and has been converted to %s \n", s.str().c_str());
    }
    const std::string s = ss.str();
    return s;
    
}
/**
 * @brief Fills a char array with values from a vector.
 * @todo add in error checking if data > 223
 * @param[in] data_vec
 * @param[out] data_char_arr 
 * 
*/
void vectorToCharArray(const std::vector<uint8_t>& data_vec, unsigned char (&data_char_arr)[223]) {
    size_t len = data_vec.size();
    for (size_t i = 0; i < len; ++i) {
        data_char_arr[i] = static_cast<unsigned char>(data_vec[i]);
    }
}

//---------------------------------------------------------------------------------------------------------------------------------------------

/**
 * \brief takes mesages out of controller queue and sends message out on that controller
 * @todo update time to take time from message
 * @todo update for multiple controllers
 * 
*/
void SendN2kMsg() {
  if (ctrl0_q.empty()){
    return;
  }
  NMEA_msg msg = ctrl0_q.front();
  ctrl0_q.pop();
  tN2kMsg N2kMsg;
  N2kMsg.Priority = msg.priority;
  N2kMsg.PGN = msg.PGN;
  N2kMsg.Source = msg.source;
  N2kMsg.Destination = 0xff; //not used

  N2kMsg.DataLen = msg.data_length_bytes;
  vectorToCharArray(msg.data, N2kMsg.Data);
  N2kMsg.MsgTime = N2kMillis64();//TODO 

  if ( NMEA2000.SendMsg(N2kMsg) ) {
    ESP_LOGI(TAG, "sent a message");
    N2kMsgSentCount++;
  } else {
    ESP_LOGI(TAG, "failed to send a message");
    N2kMsgFailCount++;
  }
}

/**
 * @brief FreeRTOS task for sending and receiving messages from CAN controller
 * 
 * @todo frame buffer should be 32 - see if this works
 * @param pvParameters
*/
void N2K_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting task");
    
    NMEA2000.SetN2kCANMsgBufSize(8);
    NMEA2000.SetN2kCANReceiveFrameBufSize(250);
    NMEA2000.EnableForward(false);                

    NMEA2000.SetMsgHandler(HandleNMEA2000Msg);

    NMEA2000.SetMode(tNMEA2000::N2km_ListenAndSend);

    NMEA2000.Open();
    for (;;)
    {
        // this runs everytime the task runs:
      SendN2kMsg();
      NMEA2000.ParseMessages();    
    }
    vTaskDelete(NULL); // should never get here...
}


/**
 * \brief Creates a NMEA_msg object and adds it to the received messages queue
 * 
 * @todo handle out of range data
 * \param N2kMsg Reference to the N2KMs being handled
 * \return void
 */
void HandleNMEA2000Msg(const tN2kMsg &N2kMsg) {
  NMEA_msg msg;
  msg.controller_number = 0;
  msg.priority = N2kMsg.Priority;
  msg.PGN = N2kMsg.PGN;
  msg.data_length_bytes = N2kMsg.DataLen;
  size_t size = sizeof(N2kMsg.Data) / sizeof(N2kMsg.Data[0]);
  // Perform the conversion with range checking
  for (size_t i = 0; i < size; i++) {
      if (N2kMsg.Data[i] <= static_cast<unsigned char>(CHAR_MAX)) {
          msg.data.push_back(static_cast<signed char>(N2kMsg.Data[i]));
      } else {
          // Handle out-of-range value
          //msg.data[i] = /* Your desired behavior for out-of-range values */;
          ESP_LOGE(TAG, "data out of range for signed array");
      }
  }

  received_msgs_q.push(msg);
  ESP_LOGI(TAG, "added msg to received queue\n");
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
        printf("%s\n", exception);
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
            "PrintInt32", // the name of WASM function name
            reinterpret_cast<void*>(PrintInt32),    // the native function pointer
            "(ii)",  // the function prototype signature, avoid to use i32
            NULL        // attachment is NULL
        },
        {
            "SendMsg", // the name of WASM function name
            reinterpret_cast<void*>(SendMsg),    // the native function pointer
            "(iiii*~)i",  // the function prototype signature, avoid to use i32
            NULL        // attachment is NULL
        },
        {
            "GetMsg", // the name of WASM function name
            reinterpret_cast<void*>(GetMsg),    // the native function pointer
            "()i",  // the function prototype signature, avoid to use i32
            NULL        // attachment is NULL
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


    ESP_LOGI(TAG, "Initialize WASM runtime");
    /* initialize runtime environment */
    if (!wasm_runtime_full_init(&init_args)) {
        ESP_LOGE(TAG, "Init runtime failed.");
        return NULL;
    }
    ESP_LOGI(TAG, "Run wamr with interpreter");

    wasm_file_buf = (uint8_t *)nmea_attack_wasm;
    wasm_file_buf_size = sizeof(nmea_attack_wasm);

    /* load WASM module */
    if (!(wasm_module = wasm_runtime_load(wasm_file_buf, wasm_file_buf_size,
                                          error_buf, sizeof(error_buf)))) {
        ESP_LOGE(TAG, "Error in wasm_runtime_load: %s", error_buf);
        goto fail;
    }

    ESP_LOGI(TAG, "Instantiate WASM runtime");
    if (!(wasm_module_inst =
              wasm_runtime_instantiate(wasm_module, 32 * 1024, // stack size
                                       32 * 1024,              // heap size
                                       error_buf, sizeof(error_buf)))) {
        ESP_LOGE(TAG, "Error while instantiating: %s", error_buf);
        goto fail;
    }

    
    exec_env = wasm_runtime_create_exec_env(wasm_module_inst, 32 * 1024);//stack size
    if (!exec_env) {
        printf("Create wasm execution environment failed.\n");
        goto fail;
    }

    ESP_LOGI(TAG, "Malloc buffer in wasm function");
    buffer_for_wasm = wasm_runtime_module_malloc(wasm_module_inst, 100, (void **)&wasm_buffer);
    if (buffer_for_wasm == 0) {
        ESP_LOGI(TAG, "Malloc failed");
        goto fail;
    }
    uint32 argv[2];
    argv[0] = buffer_for_wasm;     /* pass the buffer address for WASM space */
    argv[1] = 10+223*2;                 /* the size of buffer */
    ESP_LOGI(TAG, "Call wasm function");
    /* it is runtime embedder's responsibility to release the memory,
       unless the WASM app will free the passed pointer in its code */
    
    if (!(func = wasm_runtime_lookup_function(wasm_module_inst, "link_msg_buffer",
                                               NULL))) {
        printf(
            "The wasm function link_msg_buffer wasm function is not found.\n");
        goto fail;
    }

    if (wasm_runtime_call_wasm(exec_env, func, 2, argv)) {
        printf("Native finished calling wasm function: link_msg_buffer, "
               "returned a formatted string: %s\n",
               wasm_buffer);
    }
    else {
        printf("call wasm function link_msg_buffer failed. error: %s\n",
               wasm_runtime_get_exception(wasm_module_inst));
        goto fail;
    }

    while (true){
        ESP_LOGI(TAG, "run main() of the application");
        ret = app_instance_main(wasm_module_inst);
        assert(!ret);
        vTaskDelay(100 / portTICK_PERIOD_MS);

    }


    wasm_runtime_module_free(wasm_module_inst, buffer_for_wasm);
    
    /* destroy the module instance */
    ESP_LOGI(TAG, "Deinstantiate WASM runtime");
    wasm_runtime_deinstantiate(wasm_module_inst);

fail:
    if (exec_env)
        wasm_runtime_destroy_exec_env(exec_env);
    if (wasm_module_inst) {
        if (buffer_for_wasm){
            wasm_runtime_module_free(wasm_module_inst, buffer_for_wasm);}
        wasm_runtime_deinstantiate(wasm_module_inst);
    }
    if (wasm_module){
        /* unload the module */
        ESP_LOGI(TAG, "Unload WASM module");
        wasm_runtime_unload(wasm_module);
    }
    if (wasm_buffer)
        BH_FREE(wasm_buffer);

    /* destroy runtime environment */
    ESP_LOGI(TAG, "Destroy WASM runtime");
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
    /* Create task */
    esp_err_t result = ESP_OK;
    ESP_LOGV(TAG, "create task");
    xTaskCreate(
        &N2K_task,            // Pointer to the task entry function.
        "Sending_task",           // A descriptive name for the task for debugging.
        3072,                 // size of the task stack in bytes.
        NULL,                 // Optional pointer to pvParameters
        tskIDLE_PRIORITY, // priority at which the task should run
        &N2K_task_handle      // Optional pass back task handle
    );
    if (N2K_task_handle == NULL)
    {
        ESP_LOGE(TAG, "Unable to create task.");
        result = ESP_ERR_NO_MEM;
        goto err_out;
    }

    /* Create pthread */
    pthread_t t;
    int res;

    pthread_attr_t tattr;
    pthread_attr_init(&tattr);
    pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_JOINABLE);
    pthread_attr_setstacksize(&tattr, 4096);

    res = pthread_create(&t, &tattr, iwasm_main, (void *)NULL);
    assert(res == 0);

    res = pthread_join(t, NULL);
    assert(res == 0);


err_out:
    if (result != ESP_OK)
    {
        if (N2K_task_handle != NULL)
        {
            vTaskDelete(N2K_task_handle);
            N2K_task_handle = NULL;
        }
    }

    return 0;
};