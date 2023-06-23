#ifndef NMEA_MSG_H
#define NMEA_MSG_H
#include <vector>
struct NMEA_msg {
    uint8_t controller_number;
    static const int MaxDataLen=223;
    uint32_t PGN : 18;
    uint8_t source;
    uint8_t priority : 3;
    int data_length_bytes;
    std::vector<uint8_t> data;
};

#endif //NMEA_MSG_Hcode 