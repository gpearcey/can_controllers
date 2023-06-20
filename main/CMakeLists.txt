idf_component_register(SRCS "receive_transmit_freertos.cpp" "ESP32N2kStream.cpp" 
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp32-mcp2515 driver freertos NMEA2000 NMEA2000_esp32)