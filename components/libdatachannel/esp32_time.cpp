// ESP32 time compatibility for clock_gettime()
#include <time.h>
#include <sys/time.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Implement clock_gettime for ESP32
int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    if (!tp) return -1;
    
    switch (clk_id) {
        case CLOCK_MONOTONIC: {
            // Use ESP32's high-resolution timer
            int64_t time_us = esp_timer_get_time();
            tp->tv_sec = time_us / 1000000LL;
            tp->tv_nsec = (time_us % 1000000LL) * 1000LL;
            return 0;
        }
        
        case CLOCK_REALTIME: {
            // Use system time
            struct timeval tv;
            if (gettimeofday(&tv, nullptr) == 0) {
                tp->tv_sec = tv.tv_sec;
                tp->tv_nsec = tv.tv_usec * 1000LL;
                return 0;
            }
            return -1;
        }
        
        default:
            return -1;
    }
}

// Get monotonic time in milliseconds (common pattern in libdatachannel)
uint64_t get_monotonic_time_ms() {
    return esp_timer_get_time() / 1000LL;
}

// Get monotonic time in microseconds  
uint64_t get_monotonic_time_us() {
    return esp_timer_get_time();
}