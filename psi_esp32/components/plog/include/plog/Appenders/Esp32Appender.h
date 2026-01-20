#pragma once

#include <plog/Appenders/IAppender.h>
#include <plog/Formatters/TxtFormatter.h>
#include <plog/WinApi.h>
#include <esp_log.h>

namespace plog
{
    template<class Formatter>
    class PLOG_LINKAGE_HIDDEN Esp32Appender : public IAppender
    {
    public:
        Esp32Appender(const char* tag = "plog") : m_tag(tag)
        {
        }

        virtual void write(const Record& record) PLOG_OVERRIDE
        {
            // Format the message using the provided formatter
            util::nstring str = Formatter::format(record);
            
            // Remove trailing newline if present (ESP-IDF adds its own)
            if (!str.empty() && str.back() == '\n') {
                str.pop_back();
            }
            
            // Map plog severity to ESP-IDF log level
            esp_log_level_t esp_level;
            switch (record.getSeverity()) {
                case plog::fatal:
                case plog::error:
                    esp_level = ESP_LOG_ERROR;
                    break;
                case plog::warning:
                    esp_level = ESP_LOG_WARN;
                    break;
                case plog::info:
                    esp_level = ESP_LOG_INFO;
                    break;
                case plog::debug:
                    esp_level = ESP_LOG_DEBUG;
                    break;
                case plog::verbose:
                    esp_level = ESP_LOG_VERBOSE;
                    break;
                default:
                    esp_level = ESP_LOG_INFO;
                    break;
            }
            
            // Use ESP-IDF logging system
            esp_log_write(esp_level, m_tag, "%s", str.c_str());
        }

    private:
        const char* m_tag;
    };
}