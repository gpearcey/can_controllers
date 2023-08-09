idf_component_register(SRCS "mcp-test-2.cpp" 
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES driver freertos esp32-mcp2515 NMEA2000 wamr)

