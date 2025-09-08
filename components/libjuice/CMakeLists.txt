# ESP32-P4 libjuice component

# libjuice source files
set(SOURCES
    src/addr.c
    src/agent.c
    src/crc32.c
    src/hash.c
    src/hmac.c
    src/ice.c
    src/juice.c
    src/log.c
    src/random.c
    src/server.c
    src/stun.c
    src/timestamp.c
    src/turn.c
    src/udp.c
)

# ESP32 component registration
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS 
        include
    PRIV_INCLUDE_DIRS
        src
    REQUIRES 
        lwip
        pthread
        mbedtls
)

# Compiler definitions for libjuice
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    JUICE_USE_NETTLE=0
    JUICE_ENABLE_DEBUG=1
    NO_EPOLL=1
    ESP32_PORT=1
)

# Link-time wrapping for ESP32 compatibility
target_link_options(${COMPONENT_LIB} PUBLIC
    # File I/O -> ESP32 RNG
    -Wl,--wrap=open
    -Wl,--wrap=read
    -Wl,--wrap=close
    # Memory allocation -> PSRAM
    -Wl,--wrap=malloc
    -Wl,--wrap=free
    -Wl,--wrap=calloc
    -Wl,--wrap=realloc
)

# ESP32-specific compiler flags
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-missing-field-initializers
    -Wno-format-truncation
    -Wno-format
    -Wno-char-subscripts
)