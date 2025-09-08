// ESP32 compatibility: Missing networking constants and macros
#pragma once

// Standard privileged port boundary
#ifndef IPPORT_RESERVED
#define IPPORT_RESERVED 1024
#endif

// IPv6 address comparison macro
// LWIP uses ip6_addr_cmp but the standard uses IN6_ARE_ADDR_EQUAL
#ifndef IN6_ARE_ADDR_EQUAL
#include <lwip/ip6_addr.h>
#define IN6_ARE_ADDR_EQUAL(a, b) \
    (memcmp((a), (b), sizeof(struct in6_addr)) == 0)
#endif