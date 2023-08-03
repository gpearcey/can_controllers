idf_component_register(SRCS "spi_driver_test.cpp" 
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES driver freertos mcp2515-esp32-idf NMEA2000 wamr)

