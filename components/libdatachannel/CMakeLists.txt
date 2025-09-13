# ESP32-P4 libdatachannel component

# Source files for minimal build
set(SOURCES
    # Core implementation files
    src/impl/init.cpp
    src/impl/certificate.cpp
    src/impl/channel.cpp
    src/impl/datachannel.cpp
    src/impl/dtlssrtptransport.cpp
    src/impl/dtlstransport.cpp
    src/impl/icetransport.cpp
    src/impl/peerconnection.cpp
    src/impl/processor.cpp
    src/impl/sctptransport.cpp
    src/impl/threadpool.cpp
    src/impl/tls.cpp
    src/impl/tlstransport.cpp
    src/impl/track.cpp
    src/impl/transport.cpp
    src/impl/utils.cpp
    src/impl/logcounter.cpp
    
    # ESP32 adaptations
    psram_allocator.cpp
    esp32_netif.cpp
    esp32_time.cpp
    esp32_random.cpp
    
    # Public API
    src/candidate.cpp
    src/channel.cpp
    src/configuration.cpp
    src/datachannel.cpp
    src/description.cpp
    src/message.cpp
    src/peerconnection.cpp
    src/track.cpp
    src/global.cpp
)

# ESP32 component registration
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS 
        include
        include/rtc
        src
    PRIV_INCLUDE_DIRS
        src/impl
        include/rtc
    REQUIRES 
        pthread
        mbedtls
        lwip
        esp_netif
        esp_event
        esp_timer
        esp_wifi
    PRIV_REQUIRES
        plog
        libjuice  
        usrsctp
)

# ESP32-specific compiler flags
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-switch  # Suppress warnings about unhandled enum values in switch statements
)

# Compiler definitions
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    RTC_ENABLE_WEBSOCKET=0
    RTC_ENABLE_MEDIA=0
    USE_MBEDTLS=1
    ESP32_PORT=1
    CONFIG_MBEDTLS_SSL_PROTO_DTLS=1
)

# MbedTLS X.509 certificate writing is already enabled in ESP-IDF

# Undefine ESP-IDF's removal of deprecated MbedTLS functions
target_compile_options(${COMPONENT_LIB} PRIVATE
    -UMBEDTLS_DEPRECATED_REMOVED
)

# Force include PSRAM allocation redirects for this component only
target_compile_options(${COMPONENT_LIB} PRIVATE
    -include esp32_malloc_redirect.h
)

# ESP32-specific compiler flags
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-missing-field-initializers
)