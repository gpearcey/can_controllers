idf_component_register(SRCS "mcp-test.cpp" 
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES driver freertos mcp2515-esp32-idf NMEA2000 wamr)

