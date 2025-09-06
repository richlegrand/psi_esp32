#pragma once

// plog to ESP-IDF logging compatibility layer
#include <esp_log.h>
#include <sstream>

// Define the logging tag for libdatachannel
#define PLOG_DEFAULT_TAG "libdatachannel"

// Stream-to-string helper class
class PlogStream {
private:
    std::ostringstream oss;
    const char* tag;
    esp_log_level_t level;
    
public:
    PlogStream(const char* t, esp_log_level_t l) : tag(t), level(l) {}
    
    template<typename T>
    PlogStream& operator<<(const T& value) {
        oss << value;
        return *this;
    }
    
    ~PlogStream() {
        switch(level) {
            case ESP_LOG_ERROR:   ESP_LOGE(tag, "%s", oss.str().c_str()); break;
            case ESP_LOG_WARN:    ESP_LOGW(tag, "%s", oss.str().c_str()); break;
            case ESP_LOG_INFO:    ESP_LOGI(tag, "%s", oss.str().c_str()); break;
            case ESP_LOG_DEBUG:   ESP_LOGD(tag, "%s", oss.str().c_str()); break;
            case ESP_LOG_VERBOSE: ESP_LOGV(tag, "%s", oss.str().c_str()); break;
        }
    }
};

// plog compatibility macros
#define PLOG_FATAL    PlogStream(PLOG_DEFAULT_TAG, ESP_LOG_ERROR)
#define PLOG_ERROR    PlogStream(PLOG_DEFAULT_TAG, ESP_LOG_ERROR)
#define PLOG_WARNING  PlogStream(PLOG_DEFAULT_TAG, ESP_LOG_WARN)
#define PLOG_INFO     PlogStream(PLOG_DEFAULT_TAG, ESP_LOG_INFO)
#define PLOG_DEBUG    PlogStream(PLOG_DEFAULT_TAG, ESP_LOG_DEBUG)
#define PLOG_VERBOSE  PlogStream(PLOG_DEFAULT_TAG, ESP_LOG_VERBOSE)

// Alternative IF variants (used by some plog code)
#define PLOG_FATAL_IF(cond)    if(cond) PLOG_FATAL
#define PLOG_ERROR_IF(cond)    if(cond) PLOG_ERROR
#define PLOG_WARNING_IF(cond)  if(cond) PLOG_WARNING
#define PLOG_INFO_IF(cond)     if(cond) PLOG_INFO
#define PLOG_DEBUG_IF(cond)    if(cond) PLOG_DEBUG
#define PLOG_VERBOSE_IF(cond)  if(cond) PLOG_VERBOSE

// Logging levels for compatibility
enum plog_severity {
    PLOG_SEVERITY_FATAL = 0,
    PLOG_SEVERITY_ERROR = 1,
    PLOG_SEVERITY_WARNING = 2,
    PLOG_SEVERITY_INFO = 3,
    PLOG_SEVERITY_DEBUG = 4,
    PLOG_SEVERITY_VERBOSE = 5
};

// Initialize function (no-op for ESP-IDF)
inline void plog_init(int severity = PLOG_SEVERITY_DEBUG) {
    // ESP-IDF logging is initialized by the system
}

// Instance-based logging (if libdatachannel uses specific loggers)
template<int instance = 0>
class Logger {
public:
    static PlogStream write(esp_log_level_t level) {
        return PlogStream(PLOG_DEFAULT_TAG, level);
    }
};

// Common plog macros that might be used
#define PLOG(severity) PlogStream(PLOG_DEFAULT_TAG, (esp_log_level_t)(ESP_LOG_ERROR + severity - PLOG_SEVERITY_ERROR))
#define PLOG_(instance, severity) PlogStream(PLOG_DEFAULT_TAG, (esp_log_level_t)(ESP_LOG_ERROR + severity - PLOG_SEVERITY_ERROR))