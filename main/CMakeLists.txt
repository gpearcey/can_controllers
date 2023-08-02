idf_component_register(SRCS "mcp-test.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES driver freertos NMEA2000 mcp2515-esp32-idf wamr)

