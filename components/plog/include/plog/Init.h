#pragma once
#include <plog/Logger.h>

#ifdef ESP32_PORT
#include <plog/Appenders/Esp32Appender.h>
#include <plog/Formatters/TxtFormatter.h>
#endif

namespace plog
{
    template<int instanceId>
    PLOG_LINKAGE_HIDDEN inline Logger<instanceId>& init(Severity maxSeverity = none, IAppender* appender = NULL)
    {
        static Logger<instanceId> logger(maxSeverity);
        return appender ? logger.addAppender(appender) : logger;
    }

    inline Logger<PLOG_DEFAULT_INSTANCE_ID>& init(Severity maxSeverity = none, IAppender* appender = NULL)
    {
        return init<PLOG_DEFAULT_INSTANCE_ID>(maxSeverity, appender);
    }

#ifdef ESP32_PORT
    // ESP32-specific init functions that automatically use ESP32 appender
    inline Logger<PLOG_DEFAULT_INSTANCE_ID>& initEsp32(Severity maxSeverity = debug, const char* tag = "plog")
    {
        static Esp32Appender<TxtFormatter> esp32Appender(tag);
        return init(maxSeverity, &esp32Appender);
    }

    template<int instanceId>
    inline Logger<instanceId>& initEsp32(Severity maxSeverity = debug, const char* tag = "plog")
    {
        static Esp32Appender<TxtFormatter> esp32Appender(tag);
        return init<instanceId>(maxSeverity, &esp32Appender);
    }
#endif
}
