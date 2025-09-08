// ESP32 compatibility functions for USRSCTP
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

static const char* TAG = "usrsctp_compat";

// Minimal ifaddrs structure for ESP32
struct ifaddrs {
    struct ifaddrs  *ifa_next;    /* Next item in list */
    char            *ifa_name;    /* Name of interface */
    unsigned int     ifa_flags;   /* Flags from SIOCGIFFLAGS */
    struct sockaddr *ifa_addr;    /* Address of interface */
    struct sockaddr *ifa_netmask; /* Netmask of interface */
    union {
        struct sockaddr *ifu_broadaddr; /* Broadcast address */
        struct sockaddr *ifu_dstaddr;   /* Point-to-point destination address */
    } ifa_ifu;
    void            *ifa_data;    /* Address-specific data */
};

#define ifa_broadaddr ifa_ifu.ifu_broadaddr
#define ifa_dstaddr   ifa_ifu.ifu_dstaddr

// Simple implementation of getifaddrs for ESP32
int getifaddrs(struct ifaddrs **ifap) {
    if (!ifap) {
        errno = EINVAL;
        return -1;
    }
    
    ESP_LOGD(TAG, "getifaddrs called - returning empty interface list for ESP32");
    
    // For ESP32, return empty list
    // USRSCTP can work without specific interface enumeration
    *ifap = NULL;
    return 0;
}

void freeifaddrs(struct ifaddrs *ifa) {
    // Nothing to free in our minimal implementation
    (void)ifa;
    ESP_LOGD(TAG, "freeifaddrs called");
}

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