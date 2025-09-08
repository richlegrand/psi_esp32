#pragma once

// ESP32 compatibility header for netinet/icmp6.h
// Redirect to use LWIP's ICMPv6 definitions

#include <lwip/icmp6.h>
#include <lwip/prot/icmp6.h>