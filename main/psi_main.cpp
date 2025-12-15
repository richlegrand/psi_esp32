/**
 * PSI ESP32 WebRTC Server
 *
 * Main entry point - initializes WiFi and starts WebRTC server
 * with HTTP-like handlers over DataChannel (SWSP protocol)
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"

#include "rtc/rtc.hpp"
#include "httpd_server.hpp"
#include "httpd_test.h"
#include "esp32_psram_init.h"

static const char *TAG = "psi_main";

// WiFi credentials
#define WIFI_SSID "psinet"
#define WIFI_PASS "4053487993"
#define MAXIMUM_RETRY 5

// PSI server configuration (set via environment variables for httpd_start)
#define PSI_SERVER_URL "psi.vizycam.com"
#define DEVICE_UID "0123456789"

// FreeRTOS event group for WiFi connection
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_IPV6_BIT      BIT2

static int s_retry_num = 0;
static esp_netif_t *s_sta_netif = NULL;

//=============================================================================
// WiFi Event Handler
//=============================================================================

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;

        // Create IPv6 link-local address
        if (s_sta_netif) {
            esp_err_t ipv6_ret = esp_netif_create_ip6_linklocal(s_sta_netif);
            if (ipv6_ret == ESP_OK) {
                ESP_LOGI(TAG, "IPv6 link-local address creation initiated");
            } else {
                ESP_LOGW(TAG, "Failed to create IPv6 link-local address: %s", esp_err_to_name(ipv6_ret));
            }
        }

        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t* event = (ip_event_got_ip6_t*) event_data;
        ESP_LOGI(TAG, "Got IPv6 address: " IPV6STR, IPV62STR(event->ip6_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_IPV6_BIT);
    }
}

//=============================================================================
// WiFi Initialization
//=============================================================================

static void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_t instance_got_ip6;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_GOT_IP6,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip6));

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASS);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");
}

//=============================================================================
// LittleFS Initialization
//=============================================================================

static void littlefs_init(void) {
    ESP_LOGI(TAG, "Initializing LittleFS");

    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/littlefs";
    conf.partition_label = "storage";
    conf.format_if_mount_failed = false;
    conf.dont_mount = false;

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount LittleFS");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info("storage", &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "LittleFS partition size: total: %d KB, used: %d KB", total/1024, used/1024);
    }
}

//=============================================================================
// Main Entry Point
//=============================================================================

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Starting PSI ESP32 WebRTC Server...");

    // Initialize NVS (required for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Enable PSRAM as default malloc target
    enable_psram_malloc();

    // Initialize LittleFS for static files
    littlefs_init();

    ESP_LOGI(TAG, "After LittleFS - Internal RAM: %d KB free",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);

    // Initialize WiFi
    ESP_LOGI(TAG, "Initializing WiFi with ESP-Hosted...");
    wifi_init_sta();

    ESP_LOGI(TAG, "After WiFi init - Internal RAM: %d KB free",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);

    // Wait for WiFi connection
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID:%s", WIFI_SSID);

        // Wait for IPv6 address (with timeout)
        ESP_LOGI(TAG, "Waiting for IPv6 address...");
        EventBits_t ipv6_bits = xEventGroupWaitBits(s_wifi_event_group,
                WIFI_IPV6_BIT,
                pdFALSE,
                pdFALSE,
                pdMS_TO_TICKS(10000));

        if (ipv6_bits & WIFI_IPV6_BIT) {
            ESP_LOGI(TAG, "IPv6 link-local address acquired");
        } else {
            ESP_LOGI(TAG, "IPv6 address not available, continuing with IPv4 only");
        }

    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
        return;
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
        return;
    }

    // Initialize libdatachannel
    ESP_LOGI(TAG, "Initializing libdatachannel...");
    rtc::InitLogger(rtc::LogLevel::Info);
    rtc::StartNetworking();

    ESP_LOGI(TAG, "After libdatachannel init - Internal RAM: %d KB free",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);

    // Set environment variables for httpd_start() to use
    setenv("DEVICE_UID", DEVICE_UID, 1);
    setenv("PSI_SERVER", PSI_SERVER_URL, 1);

    // Start HTTP server (uses WebRTC DataChannel transport)
    // This is the ESP-IDF compatible API - same code works on desktop and ESP32
    esp_err_t server_ret = httpd_test_start();
    if (server_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %d", server_ret);
        return;
    }

    ESP_LOGI(TAG, "Server started! Access via: https://%s/%s", PSI_SERVER_URL, DEVICE_UID);

    // Main loop - monitor heap
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        size_t free_heap = esp_get_free_heap_size();
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "Heap: %lu KB free | Internal: %lu KB",
                 free_heap / 1024, free_internal / 1024);
        print_alloc_stats();
    }
}
