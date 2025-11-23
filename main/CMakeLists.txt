idf_component_register(
    SRCS "main.cpp" "h264fileparser.cpp" "fileparser.cpp" "opusfileparser.cpp" "stream.cpp" "dispatchqueue.cpp" "helpers.cpp" "streamer_main.cpp" "memory_tracker.cpp" "alloc_trace.c"
    INCLUDE_DIRS "."
    REQUIRES
        libdatachannel
        esp_netif
        esp_event
        nvs_flash        # Required for WiFi configuration storage
        esp_wifi         # WiFi connectivity
        esp_wifi_remote  # WiFi Remote for ESP32-C6 co-processor
        esp_hosted       # ESP-Hosted transport for ESP32-C6
        littlefs         # LittleFS for media file storage (from joltwallet/littlefs)
        json             # Built-in cJSON for WebSocket signaling
        tcp_transport        # TCP transport (includes WebSocket support)
)

# Create LittleFS image from media_files directory
# This will package all H.264 files into the 'storage' partition at build time
littlefs_create_partition_image(
    storage                                      # partition name (must match partitions.csv)
    ${CMAKE_CURRENT_SOURCE_DIR}/../media_files  # source directory  
    FLASH_IN_PROJECT                            # flash when running idf.py flash
)