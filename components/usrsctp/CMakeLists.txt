# ESP32-P4 usrsctp component

# usrsctp source files (core userland SCTP implementation)
set(SOURCES
    src/netinet/sctp_asconf.c
    src/netinet/sctp_auth.c
    src/netinet/sctp_bsd_addr.c
    src/netinet/sctp_callout.c
    src/netinet/sctp_cc_functions.c
    src/netinet/sctp_crc32.c
    src/netinet/sctp_indata.c
    src/netinet/sctp_input.c
    src/netinet/sctp_output.c
    src/netinet/sctp_pcb.c
    src/netinet/sctp_peeloff.c
    src/netinet/sctp_sha1.c
    src/netinet/sctp_ss_functions.c
    src/netinet/sctp_sysctl.c
    src/netinet/sctp_timer.c
    src/netinet/sctp_userspace.c
    src/netinet/sctp_usrreq.c
    src/netinet/sctputil.c
    src/netinet6/sctp6_usrreq.c
    src/user_environment.c
    src/user_mbuf.c
    src/user_recv_thread.c
    src/user_socket.c
    # ESP32 compatibility functions
    esp32_compat.c
)

# ESP32 component registration
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS 
        src
        include
    PRIV_INCLUDE_DIRS
        src/netinet
        src/netinet6
    REQUIRES 
        lwip
        pthread
)

# Compiler definitions for usrsctp
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    __Userspace__=1
    __Userspace_os_Linux=1
    # ESP32 as Linux-compatible userspace client
    __linux__=1
    __native_client__=1
    # Network protocol support
    INET=1
    INET6=1
    # Socket address length fields (ESP32/LWIP has all length fields)
    HAVE_SA_LEN=1
    HAVE_SIN_LEN=1  
    HAVE_SIN6_LEN=1
    # SCTP configuration for embedded
    SCTP_SIMPLE_ALLOCATOR=1
    SCTP_PROCESS_LEVEL_LOCKS=1
    SCTP_DEBUG=1
    # Prevent conflicts with LWIP definitions
    HAVE_STRUCT_IOVEC=1
    # Platform identification for ESP32
    ESP32_PORT=1
    _GNU_SOURCE=1
)

# Link-time wrapping for ESP32 compatibility  
target_link_options(${COMPONENT_LIB} PUBLIC
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
    -Wno-address-of-packed-member
    -Wno-format-truncation
    -Wno-implicit-fallthrough
    -Wno-format
)