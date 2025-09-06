idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES 
        libdatachannel
        esp_netif
        esp_event
)