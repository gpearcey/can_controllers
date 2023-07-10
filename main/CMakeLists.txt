idf_component_register(SRCS "ESP32N2kStream.cpp" "main.cpp" #"real_time_stats_example_main.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES driver freertos NMEA2000 NMEA2000_esp32-c6 wamr)

