#include "wasm_functions.h"


static const char* TAG = "WASM";

/**
 * @brief prints a uint8_t array to terminal
 * 
 * Native function to be exported to WASM app. Used for debugging purposes.
 * 
 * @param exec_env
 * @param input pointer to array
 * @param length length of the array
*/
static void PrintStr(wasm_exec_env_t exec_env,uint8_t* input, int32_t length){
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
static void PrintInt32(wasm_exec_env_t exec_env,int32_t number,int32_t hex){
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
static int32_t SendMsg(wasm_exec_env_t exec_env, int32_t controller_number, int32_t priority, int32_t PGN, int32_t source, uint8_t* data, int32_t data_length_bytes ){
    ESP_LOGD(TAG, "SendMsg called \n");
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

    
    if (controller_number == 0){
        // Add to controller 0 queue
        ESP_LOGD(TAG,"Added a msg to ctrl0_q with PGN %u \n", msg.PGN);
        if (xQueueSendToBack(controller0_tx_queue, &msg, pdMS_TO_TICKS(10))){
            return 1;
        }
    }

    return 0;
}