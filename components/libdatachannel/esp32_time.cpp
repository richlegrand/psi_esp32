// ESP32 time compatibility for clock_gettime()
#include <time.h>
#include <sys/time.h>
#include <errno.h>
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

// nanosleep implementation using FreeRTOS delay
extern "C" int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req || req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    
    // Convert to milliseconds, minimum 1ms
    int64_t total_ns = req->tv_sec * 1000000000LL + req->tv_nsec;
    int64_t delay_ms = total_ns / 1000000LL;
    if (delay_ms == 0 && total_ns > 0) {
        delay_ms = 1; // Minimum delay
    }
    
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    
    // For ESP32, we don't support interruption, so remaining time is always 0
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    
    return 0;
}

// Get monotonic time in milliseconds (common pattern in libdatachannel)
uint64_t get_monotonic_time_ms() {
    return esp_timer_get_time() / 1000LL;
}

// Get monotonic time in microseconds  
uint64_t get_monotonic_time_us() {
    return esp_timer_get_time();
}