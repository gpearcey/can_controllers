#include <N2kMsg.h>
#include <NMEA2000_esp32-c6.h> 
#include <NMEA2000.h>
#include <N2kMessages.h>

enum CAN_controller_type { twai, mcp };

class CANSend(){

public:

    CanSend(Can_controller_type ctrl_type, gpio_num_t TxPin,  gpio_num_t RxPin){
        if (ctrl_type == twai){
            tNMEA2000_esp32c6 NMEA2000(TxPin, _RxPin);
        }
        //todo add in mcp
    }

    


};