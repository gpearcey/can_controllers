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