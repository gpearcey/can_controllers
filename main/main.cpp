#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "NMEA_msg.h"
#include "esp_log.h"
#include <N2kMsg.h>
#include <NMEA2000_esp32.h> 
#include <NMEA2000.h>
#include "ESP32N2kStream.h"
#include <N2kMessages.h>
#include "driver/gpio.h"

#include "wasm_export.h"
#include "bh_read_file.h"
#include "bh_getopt.h"
#include "bh_platform.h"

/**
 * WebAssembly App
*/
#include "wasm_file.h"

using tN2kSendFunction=void (*)();
ESP32N2kStream serial;

static const char* TAG = "message_sending.cpp";
tNMEA2000_esp32 NMEA2000(GPIO_NUM_32, GPIO_NUM_34);

static TaskHandle_t N2K_task_handle = NULL;

// Structure for holding message sending information
struct tN2kSendMessage {
  tN2kSendFunction SendFunction;
  const char *const Description;
  tN2kSyncScheduler Scheduler;

  tN2kSendMessage(tN2kSendFunction _SendFunction, const char *const _Description, uint32_t /* _NextTime */ 
                  ,uint32_t _Period, uint32_t _Offset, bool _Enabled) : 
                    SendFunction(_SendFunction), 
                    Description(_Description), 
                    Scheduler(_Enabled,_Period,_Offset) {}
  void Enable(bool state);
};

extern tN2kSendMessage N2kSendMessages[];
extern size_t nN2kSendMessages;

static unsigned long N2kMsgSentCount=0;
static unsigned long N2kMsgFailCount=0;
static bool ShowSentMessages=false;
static bool Sending=false;
static tN2kScheduler NextStatsTime;

void SendN2kMsg(const tN2kMsg &N2kMsg) {
  if ( NMEA2000.SendMsg(N2kMsg) ) {
    N2kMsgSentCount++;
  } else {
    N2kMsgFailCount++;
  }
  if ( ShowSentMessages ) N2kMsg.Print(&serial);
}
void SendN2kPressure() {
    tN2kMsg N2kMsg;
    N2kMsg.SetPGN(130314L);
    N2kMsg.Priority = 5;
    N2kMsg.AddByte(0);
    N2kMsg.AddByte((unsigned char) 2);
    N2kMsg.AddByte((unsigned char) N2kps_CompressedAir);
    N2kMsg.Add4ByteDouble(mBarToPascal(1024),0.1);
    N2kMsg.AddByte(0xff); // reserved
    SendN2kMsg(N2kMsg);
}
// *****************************************************************************
// Call back for NMEA2000 open. This will be called, when library starts bus communication.
// See NMEA2000.SetOnOpen(OnN2kOpen); on setup()
// We initialize here all messages next sending time. Since we use tN2kSyncScheduler all messages
// send offset will be synchronized to libary.
void OnN2kOpen() {
  for ( size_t i=0; i<nN2kSendMessages; i++ ) {
    if ( N2kSendMessages[i].Scheduler.IsEnabled() ) N2kSendMessages[i].Scheduler.UpdateNextTime();
  }
  Sending=true;
}
// *****************************************************************************
// Function check is it time to send message. If it is, message will be sent and
// next send time will be updated.
// Function always returns next time it should be handled.
int64_t CheckSendMessage(tN2kSendMessage &N2kSendMessage) {
  if ( N2kSendMessage.Scheduler.IsDisabled() ) return N2kSendMessage.Scheduler.GetNextTime();

  if ( N2kSendMessage.Scheduler.IsTime() ) {
    N2kSendMessage.Scheduler.UpdateNextTime();
    N2kSendMessage.SendFunction();
  }

  return N2kSendMessage.Scheduler.GetNextTime();
}

// *****************************************************************************
// Function send enabled messages from tN2kSendMessage structure according to their
// period+offset.
void SendN2kMessages() {
  static uint64_t NextSend=0;
  uint64_t Now=N2kMillis64();

  if ( NextSend<Now ) {
    uint64_t NextMsgSend;
    NextSend=Now+2000;
    for ( size_t i=0; i<nN2kSendMessages; i++ ) {
      NextMsgSend=CheckSendMessage(N2kSendMessages[i]);
      if ( NextMsgSend<NextSend ) NextSend=NextMsgSend;
    }
  }
}

// This is a FreeRTOS task
void N2K_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting task");
    
    NMEA2000.SetN2kCANMsgBufSize(8);
    NMEA2000.SetN2kCANReceiveFrameBufSize(250);// not sure what this should be set to

    NMEA2000.SetForwardStream(new ESP32N2kStream()); // will need to change to WASM expected format
    NMEA2000.SetForwardType(tNMEA2000::fwdt_Text);   // Show in clear text
    // NMEA2000.EnableForward(false);                 // Disable all msg forwarding to USB (=Serial)

    //NMEA2000.SetMsgHandler(HandleNMEA2000Msg); //optional message handler will do somthing once desired message is received

    //do I need to do anything for group functions?

    // If you also want to see all traffic on the bus use N2km_ListenAndNode instead of N2km_NodeOnly below
    NMEA2000.SetMode(tNMEA2000::N2km_ListenAndSend, 22);//does 22 do anything in ListenOnly mode?

    // Here we could tell, which PGNs we transmit and receive
    //NMEA2000.ExtendTransmitMessages(TransmitMessages, 0);
    //NMEA2000.ExtendReceiveMessages(ReceiveMessages, 0);

    // Define OnOpen call back. This will be called, when CAN is open and system starts address claiming.
    NMEA2000.SetOnOpen(OnN2kOpen);
    NMEA2000.Open();
    for (;;)
    {
        // this runs everytime the task runs:
      SendN2kMessages();
      NMEA2000.ParseMessages();    
    }
    vTaskDelete(NULL); // should never get here...
}
#define AddSendPGN(fn,NextSend,Period,Offset,Enabled) {fn,#fn,NextSend,Period,Offset+300,Enabled}
tN2kSendMessage N2kSendMessages[]={
    AddSendPGN(SendN2kPressure,0,2000,42,true) // 130314
};

size_t nN2kSendMessages=sizeof(N2kSendMessages)/sizeof(tN2kSendMessage);

/**************************************************************************************************
 * Wasm task
*/
#define NATIVE_STACK_SIZE (4*1024)

int printHello(wasm_exec_env_t exec_env,int32_t number ){
    printf("Hello World #%ld \n", number);
    return 0;
}
static void * app_instance_main(wasm_module_inst_t module_inst)
{
    const char *exception;

    wasm_application_execute_main(module_inst, 0, NULL);
    if ((exception = wasm_runtime_get_exception(module_inst)))
        printf("%s\n", exception);
    return NULL;
}

void * iwasm_main(void *arg)
{
    (void)arg; /* unused */
    /* setup variables for instantiating and running the wasm module */
    uint8_t *wasm_file_buf = NULL;
    unsigned wasm_file_buf_size = 0;
    wasm_module_t wasm_module = NULL;
    wasm_module_inst_t wasm_module_inst = NULL;
    char error_buf[128];
    void *ret;
    RuntimeInitArgs init_args;

    /* configure memory allocation */
    memset(&init_args, 0, sizeof(RuntimeInitArgs));

    static NativeSymbol native_symbols[] = {
        {
            "printHello", // the name of WASM function name
            reinterpret_cast<void*>(printHello),    // the native function pointer
            "(i)i",  // the function prototype signature, avoid to use i32
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

    // Native symbols need below registration phase
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

    wasm_file_buf = (uint8_t *)wasm_project_wasm;
    wasm_file_buf_size = sizeof(wasm_project_wasm);

    /* load WASM module */
    if (!(wasm_module = wasm_runtime_load(wasm_file_buf, wasm_file_buf_size,
                                          error_buf, sizeof(error_buf)))) {
        ESP_LOGE(TAG, "Error in wasm_runtime_load: %s", error_buf);
        goto fail1interp;
    }

    ESP_LOGI(TAG, "Instantiate WASM runtime");
    if (!(wasm_module_inst =
              wasm_runtime_instantiate(wasm_module, 32 * 1024, // stack size
                                       32 * 1024,              // heap size
                                       error_buf, sizeof(error_buf)))) {
        ESP_LOGE(TAG, "Error while instantiating: %s", error_buf);
        goto fail2interp;
    }

    ESP_LOGI(TAG, "run main() of the application");
    ret = app_instance_main(wasm_module_inst);
    assert(!ret);

    
    /* destroy the module instance */
    ESP_LOGI(TAG, "Deinstantiate WASM runtime");
    wasm_runtime_deinstantiate(wasm_module_inst);

fail2interp:
    /* unload the module */
    ESP_LOGI(TAG, "Unload WASM module");
    wasm_runtime_unload(wasm_module);
fail1interp:

    /* destroy runtime environment */
    ESP_LOGI(TAG, "Destroy WASM runtime");
    wasm_runtime_destroy();

    return NULL;
}

extern "C" int app_main(void)
{
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

    //wasm task must run on a pthread - just a wrapper on FreeRTOS
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