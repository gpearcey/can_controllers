#ifndef MSG_CONVERTER_H
#define MSG_CONVERTER_H

#include "N2MEA_msg.h"


class MsgConverter{
    // Returns first byte if incoming data, or -1 on no available data.
    int read() override;
    int peek() override;

    // Write data to stream.
    size_t write(const uint8_t* data, size_t size) override;
private:
    char buff[2048];
    int  len = 0;
public:
    void println();
    void println(long l);
    void println(char *l);
    void println(const char* format, ...);
};


#endif //MSG_CONVERTER_H