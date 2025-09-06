// Compatibility header for BSD sys/queue.h on ESP32
#ifndef _SYS_QUEUE_H_
#define _SYS_QUEUE_H_

// Include ESP-IDF's sys/queue.h which provides BSD queue macros
#include_next <sys/queue.h>

// Add any missing definitions if needed
#ifndef TAILQ_FOREACH_SAFE
#define TAILQ_FOREACH_SAFE(var, head, field, tvar)                      \
        for ((var) = TAILQ_FIRST((head));                              \
            (var) != TAILQ_END((head)) &&                              \
            ((tvar) = TAILQ_NEXT((var), field), 1);                    \
            (var) = (tvar))
#endif

#endif /* _SYS_QUEUE_H_ */