#pragma once

// ESP32 compatibility header for sys/prctl.h
// ESP-IDF doesn't provide Linux process control interface

// Thread naming constant - most common use case
#ifndef PR_SET_NAME
#define PR_SET_NAME 15  /* Set thread name */
#endif

// Stub implementation - ESP-IDF doesn't support prctl
// Returns success but operations are no-ops (fine for thread naming)
static inline int prctl(int option, ...) {
    (void)option;  // Suppress unused parameter warning
    return 0;      // Success
}