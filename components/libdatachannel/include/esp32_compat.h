#pragma once

// ESP32-P4 compatibility layer for libdatachannel
// Maps standard POSIX/C++ to ESP-IDF components

// Threading - ESP-IDF has full pthread and std::thread support
#include <pthread.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>

// Sockets - LWIP provides POSIX socket API with poll()
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/dns.h>

// poll() is available via LWIP
#include <sys/poll.h>

// ESP-IDF specific includes
#include <esp_log.h>
#include <esp_system.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// MbedTLS for crypto
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509.h>
#include <mbedtls/pk.h>

// Memory management helpers
#define RTC_MALLOC(size) heap_caps_malloc(size, MALLOC_CAP_DEFAULT)
#define RTC_FREE(ptr) heap_caps_free(ptr)

// Logging macros mapping to ESP_LOG
#define RTC_LOG_TAG "libdatachannel"
#define PLOG_ERROR   ESP_LOGE(RTC_LOG_TAG, 
#define PLOG_WARNING ESP_LOGW(RTC_LOG_TAG,
#define PLOG_INFO    ESP_LOGI(RTC_LOG_TAG,
#define PLOG_DEBUG   ESP_LOGD(RTC_LOG_TAG,
#define PLOG_VERBOSE ESP_LOGV(RTC_LOG_TAG,

// Platform-specific configuration
#define RTC_ESP32_PORT 1
#define RTC_ENABLE_WEBSOCKET 0
#define RTC_ENABLE_MEDIA 0

// Thread stack sizes for ESP32
#define RTC_THREAD_STACK_SIZE (8192)
#define RTC_WORKER_STACK_SIZE (4096)

// Network buffer sizes optimized for ESP32
#define RTC_RECV_BUFFER_SIZE (4096)
#define RTC_SEND_BUFFER_SIZE (4096)

// Certificate generation
#define RTC_USE_MBEDTLS 1

// Compatibility for missing functions
#ifdef __cplusplus
extern "C" {
#endif

// Add any missing POSIX functions here if needed

#ifdef __cplusplus
}
#endif