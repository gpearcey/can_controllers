#include "wasm_export.h"
#include <stdio.h>
#include "esp_log.h"
#include "NMEA_msg.h"

class WasmFunctions{

public:

    /**
     * @brief prints a uint8_t array to terminal
     * 
     * Native function to be exported to WASM app. Used for debugging purposes.
     * 
     * @param exec_env
     * @param input pointer to array
     * @param length length of the array
    */
    static void PrintStr(wasm_exec_env_t exec_env,uint8_t* input, int32_t length);

    /**
     * @brief prints a integer to terminal
     * 
     * Native function to be exported to WASM app. Used for debugging purposes.
     * 
     * @param exec_env
     * @param number integer to print
     * @param hex prints the number in hex format if hex is 1, else prints it in decimal format
    */
    static void PrintInt32(wasm_exec_env_t exec_env,int32_t number,int32_t hex);

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
    static int32_t SendMsg(wasm_exec_env_t exec_env, int32_t controller_number, int32_t priority, int32_t PGN, int32_t source, uint8_t* data, int32_t data_length_bytes );
};