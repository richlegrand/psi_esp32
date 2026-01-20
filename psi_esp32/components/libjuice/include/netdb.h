// ESP32 compatibility: Missing netdb constants for getnameinfo()
#pragma once

#include_next <netdb.h>

// getnameinfo() flags - missing in ESP32's netdb.h but required by RFC 3493
// These are the standard POSIX values used across all platforms

#ifndef NI_NUMERICHOST
#define NI_NUMERICHOST  0x01    // Return numeric IP instead of hostname
#endif

#ifndef NI_NUMERICSERV
#define NI_NUMERICSERV  0x02    // Return port number instead of service name
#endif

#ifndef NI_NOFQDN
#define NI_NOFQDN       0x04    // Return only hostname portion for local hosts
#endif

#ifndef NI_NAMEREQD
#define NI_NAMEREQD     0x08    // Return error if hostname can't be resolved
#endif

#ifndef NI_DGRAM
#define NI_DGRAM        0x10    // Indicates datagram service
#endif