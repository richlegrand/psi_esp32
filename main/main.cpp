// ESP32-P4 libdatachannel H.264 WebRTC streamer
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"  // ESP-Hosted WiFi Remote API
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_littlefs.h"  // From joltwallet/littlefs component
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

// WebRTC streaming functionality
#include "rtc/rtc.hpp"
#include "streamer.hpp"

// ESP32 PSRAM configuration
#include "esp32_psram_init.h"

static const char *TAG = "h264_streamer";

// WiFi credentials - replace with your network
#define WIFI_SSID "psinet"
#define WIFI_PASS "4053487993"
#define MAXIMUM_RETRY 5

// FreeRTOS event group for WiFi connection
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_IPV6_BIT      BIT2

static int s_retry_num = 0;
static esp_netif_t *s_sta_netif = NULL;

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

        // Now that the interface is up, create IPv6 link-local address
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

// Initialize WiFi with IPv6 support (ESP-Hosted handles remote WiFi transparently)
static void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default WiFi station (ESP-Hosted redirects to ESP32-C6)
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

    // Use standard WiFi APIs (ESP-Hosted redirects to ESP32-C6 automatically)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");
}

// Initialize LittleFS
static void littlefs_init(void) {
    ESP_LOGI(TAG, "Initializing LittleFS");

    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = false,  // Don't format - we have media files
        .dont_mount = false,
    };

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

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Starting H.264 WebRTC streamer...");

    // Configure pthread to use PSRAM for thread stacks (critical for usrsctp)
    esp32_configure_pthread_psram();

    // Initialize NVS (required for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize LittleFS for H.264 media files
    littlefs_init();

    // Initialize WiFi with ESP-Hosted
    ESP_LOGI(TAG, "Initializing WiFi with ESP-Hosted...");
    wifi_init_sta();

    // Wait for IPv4 connection first
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", WIFI_SSID);

        // Wait for IPv6 address (with timeout since it's optional)
        ESP_LOGI(TAG, "Waiting for IPv6 address...");
        EventBits_t ipv6_bits = xEventGroupWaitBits(s_wifi_event_group,
                WIFI_IPV6_BIT,
                pdFALSE,
                pdFALSE,
                pdMS_TO_TICKS(10000)); // 10 second timeout

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

    // Test DNS resolution before WebRTC initialization
    ESP_LOGI(TAG, "Testing DNS resolution...");
    struct hostent *he = gethostbyname("google.com");
    if (he != NULL) {
        ESP_LOGI(TAG, "DNS test SUCCESS: google.com resolved to %s",
                 inet_ntoa(*(struct in_addr*)he->h_addr_list[0]));
    } else {
        ESP_LOGE(TAG, "DNS test FAILED: Could not resolve google.com");
    }
    ESP_LOGI(TAG, "DNS test completed");

    // Initialize libdatachannel for WebRTC
    rtc::InitLogger(rtc::LogLevel::Info);
    ESP_LOGI(TAG, "InitLogger called successfully");

    // ESP32: Start networking threads now that FreeRTOS scheduler is running
    ESP_LOGI(TAG, "Starting networking threads...");
    rtc::StartNetworking();
    ESP_LOGI(TAG, "Networking threads started successfully");

    // Start the H.264 WebRTC streamer
    ESP_LOGI(TAG, "Starting H.264 streamer...");
    startStreamer();

    // The streamer runs in its own context, main task can exit
    ESP_LOGI(TAG, "H.264 streamer started successfully");
}