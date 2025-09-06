// ESP32 network interface compatibility for getifaddrs()
#include <ifaddrs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <cstdlib>
#include <cstring>

static const char* TAG = "esp32_netif";

// Implementation of getifaddrs() using ESP-IDF's esp_netif
int getifaddrs(struct ifaddrs **ifap) {
    if (!ifap) {
        return -1;
    }
    
    *ifap = nullptr;
    struct ifaddrs *head = nullptr;
    struct ifaddrs *current = nullptr;
    
    // Get WiFi station interface
    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            
            // Allocate ifaddrs structure
            struct ifaddrs *ifa = (struct ifaddrs*)malloc(sizeof(struct ifaddrs));
            if (!ifa) {
                ESP_LOGE(TAG, "Failed to allocate ifaddrs");
                freeifaddrs(head);
                return -1;
            }
            
            memset(ifa, 0, sizeof(struct ifaddrs));
            
            // Interface name
            ifa->ifa_name = strdup("wlan0");
            
            // Interface flags
            ifa->ifa_flags = IFF_UP | IFF_RUNNING | IFF_BROADCAST | IFF_MULTICAST;
            
            // IPv4 address
            struct sockaddr_in *addr = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
            if (addr) {
                memset(addr, 0, sizeof(struct sockaddr_in));
                addr->sin_family = AF_INET;
                addr->sin_addr.s_addr = ip_info.ip.addr;
                ifa->ifa_addr = (struct sockaddr*)addr;
            }
            
            // Netmask
            struct sockaddr_in *netmask = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
            if (netmask) {
                memset(netmask, 0, sizeof(struct sockaddr_in));
                netmask->sin_family = AF_INET;
                netmask->sin_addr.s_addr = ip_info.netmask.addr;
                ifa->ifa_netmask = (struct sockaddr*)netmask;
            }
            
            // Add to list
            if (!head) {
                head = ifa;
                current = ifa;
            } else {
                current->ifa_next = ifa;
                current = ifa;
            }
            
            ESP_LOGI(TAG, "Added WiFi STA interface: " IPSTR, IP2STR(&ip_info.ip));
        }
    }
    
    // Get WiFi AP interface (if enabled)
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
            
            struct ifaddrs *ifa = (struct ifaddrs*)malloc(sizeof(struct ifaddrs));
            if (!ifa) {
                ESP_LOGE(TAG, "Failed to allocate AP ifaddrs");
                freeifaddrs(head);
                return -1;
            }
            
            memset(ifa, 0, sizeof(struct ifaddrs));
            
            ifa->ifa_name = strdup("wlan1");
            ifa->ifa_flags = IFF_UP | IFF_RUNNING | IFF_BROADCAST | IFF_MULTICAST;
            
            struct sockaddr_in *addr = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
            if (addr) {
                memset(addr, 0, sizeof(struct sockaddr_in));
                addr->sin_family = AF_INET;
                addr->sin_addr.s_addr = ip_info.ip.addr;
                ifa->ifa_addr = (struct sockaddr*)addr;
            }
            
            struct sockaddr_in *netmask = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
            if (netmask) {
                memset(netmask, 0, sizeof(struct sockaddr_in));
                netmask->sin_family = AF_INET;
                netmask->sin_addr.s_addr = ip_info.netmask.addr;
                ifa->ifa_netmask = (struct sockaddr*)netmask;
            }
            
            if (!head) {
                head = ifa;
                current = ifa;
            } else {
                current->ifa_next = ifa;
                current = ifa;
            }
            
            ESP_LOGI(TAG, "Added WiFi AP interface: " IPSTR, IP2STR(&ip_info.ip));
        }
    }
    
    // Add loopback interface
    struct ifaddrs *lo = (struct ifaddrs*)malloc(sizeof(struct ifaddrs));
    if (lo) {
        memset(lo, 0, sizeof(struct ifaddrs));
        lo->ifa_name = strdup("lo");
        lo->ifa_flags = IFF_UP | IFF_RUNNING | IFF_LOOPBACK;
        
        struct sockaddr_in *addr = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
        if (addr) {
            memset(addr, 0, sizeof(struct sockaddr_in));
            addr->sin_family = AF_INET;
            addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            lo->ifa_addr = (struct sockaddr*)addr;
        }
        
        struct sockaddr_in *netmask = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
        if (netmask) {
            memset(netmask, 0, sizeof(struct sockaddr_in));
            netmask->sin_family = AF_INET;
            netmask->sin_addr.s_addr = htonl(0xFF000000);
            lo->ifa_netmask = (struct sockaddr*)netmask;
        }
        
        if (!head) {
            head = lo;
        } else {
            current->ifa_next = lo;
        }
    }
    
    *ifap = head;
    return 0;
}

// Free the interface list
void freeifaddrs(struct ifaddrs *ifa) {
    while (ifa) {
        struct ifaddrs *next = ifa->ifa_next;
        
        if (ifa->ifa_name) free(ifa->ifa_name);
        if (ifa->ifa_addr) free(ifa->ifa_addr);
        if (ifa->ifa_netmask) free(ifa->ifa_netmask);
        if (ifa->ifa_broadaddr) free(ifa->ifa_broadaddr);
        
        free(ifa);
        ifa = next;
    }
}