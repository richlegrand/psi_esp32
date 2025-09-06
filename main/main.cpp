#include <stdio.h>
#include <thread>
#include <memory>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_event.h"

// Test libdatachannel includes
#include "rtc/rtc.hpp"

static const char *TAG = "main";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "ESP32-P4 libdatachannel test starting...");
    
    // Initialize network stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());
    
    try {
        // Test basic libdatachannel initialization
        rtc::InitLogger(rtc::LogLevel::Info);
        ESP_LOGI(TAG, "libdatachannel logger initialized");
        
        // Test PeerConnection creation
        rtc::Configuration config;
        auto pc = std::make_shared<rtc::PeerConnection>(config);
        ESP_LOGI(TAG, "PeerConnection created successfully");
        
        ESP_LOGI(TAG, "Basic libdatachannel test passed!");
        
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "libdatachannel test failed: %s", e.what());
    }
    
    // Print memory usage
    ESP_LOGI(TAG, "Final free heap: %lu bytes", esp_get_free_heap_size());
    
    // Keep running
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
    }
}