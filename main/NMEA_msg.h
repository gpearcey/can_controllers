#ifndef NMEA_MSG_H
#define NMEA_MSG_H
#include <vector>

struct NMEA_msg {
    uint32_t PGN : 18;
    uint8_t src;
    uint8_t priority : 3;
    int length;
    std::vector<uint8_t> data;
};

#endif //NMEA_MSG_Hcode 