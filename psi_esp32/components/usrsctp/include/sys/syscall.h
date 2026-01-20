#pragma once

// ESP32 compatibility header for sys/syscall.h
// ESP-IDF doesn't provide Linux syscall interface, but we can define what we need

// Since ESP32 doesn't have getrandom syscall, don't define __NR_getrandom
// This will cause the Linux random generation code to fall back to /dev/urandom
// which ESP-IDF does support