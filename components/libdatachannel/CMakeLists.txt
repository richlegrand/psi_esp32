# ESP32-P4 libdatachannel component

# Source files for minimal build
set(SOURCES
    # Core implementation files
    src/impl/init.cpp
    src/impl/certificate.cpp
    src/impl/channel.cpp
    src/impl/datachannel.cpp
    src/impl/dtlstransport.cpp
    src/impl/icetransport.cpp
    src/impl/peerconnection.cpp
    src/impl/processor.cpp
    src/impl/sctptransport.cpp
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
    src/global.cpp
)

# ESP32 component registration
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS 
        include
        src
    PRIV_INCLUDE_DIRS
        src/impl
    REQUIRES 
        pthread
        mbedtls
        lwip
        esp_netif
        esp_event
    PRIV_REQUIRES
        plog
        libjuice  
        usrsctp
)

# Compiler definitions
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    RTC_ENABLE_WEBSOCKET=0
    RTC_ENABLE_MEDIA=0
    USE_MBEDTLS=1
    ESP32_PORT=1
)

# Link-time wrapping for ESP32 compatibility (only for this component)
target_link_options(${COMPONENT_LIB} PUBLIC
    # Memory allocation -> PSRAM
    -Wl,--wrap=malloc
    -Wl,--wrap=free
    -Wl,--wrap=calloc
    -Wl,--wrap=realloc
    -Wl,--wrap=_Znwm     # operator new(size_t)
    -Wl,--wrap=_Znam     # operator new[](size_t) 
    -Wl,--wrap=_ZdlPv    # operator delete(void*)
    -Wl,--wrap=_ZdaPv    # operator delete[](void*)
    # File I/O -> ESP32 RNG
    -Wl,--wrap=open
    -Wl,--wrap=read
    -Wl,--wrap=close
)

# ESP32-specific compiler flags
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-missing-field-initializers
)