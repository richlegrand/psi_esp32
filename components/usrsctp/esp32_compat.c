// ESP32 compatibility functions for USRSCTP
#include <time.h>
#include <errno.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

static const char* TAG = "usrsctp_compat";

// Note: getifaddrs() and freeifaddrs() are provided by libdatachannel/esp32_sockutils.cpp

// nanosleep implementation using FreeRTOS delay
int nanosleep(const struct timespec *req, struct timespec *rem) {
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
    
    ESP_LOGD(TAG, "nanosleep: sleeping for %lld ms", delay_ms);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    
    // For ESP32, we don't support interruption, so remaining time is always 0
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    
    return 0;
}