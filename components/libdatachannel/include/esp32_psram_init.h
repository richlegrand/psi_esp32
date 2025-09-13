// ESP32 PSRAM initialization for libdatachannel
// Call esp32_configure_pthread_psram() early in app_main to use PSRAM for thread stacks

#ifndef ESP32_PSRAM_INIT_H
#define ESP32_PSRAM_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

// Configure pthread to use PSRAM for thread stacks
// This is critical for usrsctp which creates many threads
void esp32_configure_pthread_psram(void);

// Print memory statistics
void print_rtc_memory_stats(void);

#ifdef __cplusplus
}
#endif

#endif // ESP32_PSRAM_INIT_H