#pragma once

// ESP32 netdb.h compatibility for libdatachannel
// Provides missing getnameinfo() constants

// Include ESP-IDF's netdb.h first
#include <netdb.h>

// Add missing getnameinfo() flags - standard POSIX values
#ifndef NI_NUMERICHOST
#define NI_NUMERICHOST 1   /* Return numeric hostname instead of resolving */
#endif

#ifndef NI_NUMERICSERV
#define NI_NUMERICSERV 2   /* Return numeric service instead of resolving */
#endif