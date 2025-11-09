// ESP32 socket utilities with correct POSIX struct layout
// Provides: getifaddrs(), freeifaddrs(), getnameinfo(), socketpair(), pipe()
// Uses esp_netif_next_unsafe() to enumerate all interfaces including ESP-Hosted virtual interfaces

#include <ifaddrs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <esp_netif.h>
#include <esp_log.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <errno.h>

// getnameinfo() flags - standard POSIX values
#ifndef NI_NUMERICHOST
#define NI_NUMERICHOST  0x01
#endif
#ifndef NI_NUMERICSERV
#define NI_NUMERICSERV  0x02
#endif
#ifndef NI_NOFQDN
#define NI_NOFQDN       0x04
#endif
#ifndef NI_NAMEREQD
#define NI_NAMEREQD     0x08
#endif
#ifndef NI_DGRAM
#define NI_DGRAM        0x10
#endif

// EAI error codes - standard POSIX values
#ifndef EAI_OVERFLOW
#define EAI_OVERFLOW    -12
#endif

// AF_UNIX domain - used for socketpair API compatibility
#ifndef AF_UNIX
#define AF_UNIX         1
#endif

static const char* TAG = "esp32_sockutils";

// Implementation of getifaddrs() using ESP-IDF's esp_netif
extern "C" int getifaddrs(struct ifaddrs **ifap) {
    if (!ifap) {
        ESP_LOGE(TAG, "ifap is NULL");
        return -1;
    }

    *ifap = nullptr;
    struct ifaddrs *head = nullptr;
    struct ifaddrs *current = nullptr;

    // Iterate through all network interfaces using esp_netif_next_unsafe()
    esp_netif_t *netif = nullptr;
    while ((netif = esp_netif_next_unsafe(netif)) != nullptr) {

        // Get interface name
        char ifname[6];
        esp_err_t err = esp_netif_get_netif_impl_name(netif, ifname);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "Failed to get interface name: %s", esp_err_to_name(err));
            continue;
        }

        // Check if interface is up
        bool is_up = esp_netif_is_netif_up(netif);

        // Get IPv4 address
        esp_netif_ip_info_t ip_info;
        err = esp_netif_get_ip_info(netif, &ip_info);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "Failed to get IP info for %s: %s", ifname, esp_err_to_name(err));
            // Continue anyway - interface might not have IP yet
        }

        // Allocate ifaddrs structure
        struct ifaddrs *ifa = (struct ifaddrs*)malloc(sizeof(struct ifaddrs));
        if (!ifa) {
            ESP_LOGE(TAG, "Failed to allocate ifaddrs for %s", ifname);
            freeifaddrs(head);
            return -1;
        }
        memset(ifa, 0, sizeof(struct ifaddrs));

        // Set interface name
        ifa->ifa_name = strdup(ifname);
        if (!ifa->ifa_name) {
            ESP_LOGE(TAG, "Failed to duplicate interface name");
            free(ifa);
            freeifaddrs(head);
            return -1;
        }

        // Set interface flags (CORRECT POSIX field order: flags before addr!)
        ifa->ifa_flags = 0;
        if (is_up) {
            ifa->ifa_flags |= IFF_UP | IFF_RUNNING;
        }
        // Most WiFi interfaces support broadcast and multicast
        ifa->ifa_flags |= IFF_BROADCAST | IFF_MULTICAST;

        // Set IPv4 address (if available)
        if (err == ESP_OK && ip_info.ip.addr != 0) {
            struct sockaddr_in *addr = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
            if (addr) {
                memset(addr, 0, sizeof(struct sockaddr_in));
                addr->sin_family = AF_INET;
                addr->sin_addr.s_addr = ip_info.ip.addr;
                ifa->ifa_addr = (struct sockaddr*)addr;

                char ip_str[16];
                esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
                ESP_LOGD(TAG, "%s: IPv4=%s, flags=0x%x", ifname, ip_str, ifa->ifa_flags);
            } else {
                ESP_LOGE(TAG, "Failed to allocate sockaddr_in for %s", ifname);
            }

            // Set netmask
            struct sockaddr_in *netmask = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
            if (netmask) {
                memset(netmask, 0, sizeof(struct sockaddr_in));
                netmask->sin_family = AF_INET;
                netmask->sin_addr.s_addr = ip_info.netmask.addr;
                ifa->ifa_netmask = (struct sockaddr*)netmask;
            }
        } else {
            ESP_LOGD(TAG, "%s: no IPv4 address, flags=0x%x", ifname, ifa->ifa_flags);
        }

        // Add to linked list
        if (!head) {
            head = ifa;
            current = ifa;
        } else {
            current->ifa_next = ifa;
            current = ifa;
        }
    }

    // Always add loopback interface
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
            netmask->sin_addr.s_addr = htonl(0xFF000000);  // 255.0.0.0
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
extern "C" void freeifaddrs(struct ifaddrs *ifa) {
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

// Implementation of getnameinfo() for IPv4 and IPv6
extern "C" int getnameinfo(const struct sockaddr *addr, socklen_t addrlen,
                           char *host, socklen_t hostlen,
                           char *serv, socklen_t servlen, int flags) {
    // Validate input
    if (addr == NULL) {
        return EAI_FAIL;
    }

    // Validate flags
    if (flags & ~(NI_NUMERICHOST | NI_NUMERICSERV | NI_DGRAM | NI_NAMEREQD | NI_NOFQDN)) {
        return EAI_BADFLAGS;
    }

    // Handle IPv4
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;

        // Validate address length
        if (addrlen < sizeof(struct sockaddr_in)) {
            return EAI_FAMILY;
        }

        // Convert host address to string
        if (host && hostlen > 0) {
            if (flags & NI_NUMERICHOST) {
                if (inet_ntop(AF_INET, &sin->sin_addr, host, hostlen) == NULL) {
                    return EAI_OVERFLOW;
                }
            } else {
                // No reverse DNS lookup support - just return numeric
                if (inet_ntop(AF_INET, &sin->sin_addr, host, hostlen) == NULL) {
                    return EAI_OVERFLOW;
                }
            }
        }

        // Convert service (port) to string
        if (serv && servlen > 0) {
            if (flags & NI_NUMERICSERV) {
                int port = ntohs(sin->sin_port);
                if (snprintf(serv, servlen, "%d", port) >= (int)servlen) {
                    return EAI_OVERFLOW;
                }
            } else {
                // No service name lookup support - just return numeric
                int port = ntohs(sin->sin_port);
                if (snprintf(serv, servlen, "%d", port) >= (int)servlen) {
                    return EAI_OVERFLOW;
                }
            }
        }

        return 0;
    }

    // Handle IPv6
    if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)addr;

        // Validate address length
        if (addrlen < sizeof(struct sockaddr_in6)) {
            return EAI_FAMILY;
        }

        // Convert host address to string
        if (host && hostlen > 0) {
            if (flags & NI_NUMERICHOST) {
                if (inet_ntop(AF_INET6, &sin6->sin6_addr, host, hostlen) == NULL) {
                    return EAI_OVERFLOW;
                }
            } else {
                // No reverse DNS lookup support - just return numeric
                if (inet_ntop(AF_INET6, &sin6->sin6_addr, host, hostlen) == NULL) {
                    return EAI_OVERFLOW;
                }
            }
        }

        // Convert service (port) to string
        if (serv && servlen > 0) {
            if (flags & NI_NUMERICSERV) {
                int port = ntohs(sin6->sin6_port);
                if (snprintf(serv, servlen, "%d", port) >= (int)servlen) {
                    return EAI_OVERFLOW;
                }
            } else {
                // No service name lookup support - just return numeric
                int port = ntohs(sin6->sin6_port);
                if (snprintf(serv, servlen, "%d", port) >= (int)servlen) {
                    return EAI_OVERFLOW;
                }
            }
        }

        return 0;
    }

    // Unsupported address family
    return EAI_FAMILY;
}

// Implementation of socketpair() using TCP loopback
extern "C" int socketpair(int domain, int type, int protocol, int sv[2]) {
    if (protocol != 0 || type != SOCK_STREAM || domain != AF_UNIX) {
        errno = ENOSYS; // not implemented
        return -1;
    }

    const int INVALID_SOCKET = -1;
    struct sockaddr_in sa;
    socklen_t sa_len = sizeof(sa);
    int listenfd = INVALID_SOCKET;
    int fd1 = INVALID_SOCKET;
    int fd2 = INVALID_SOCKET;

    // Create listening socket on loopback
    listenfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenfd == INVALID_SOCKET) {
        ESP_LOGE(TAG, "Cannot create listening socket");
        return -1;
    }

    // Bind to loopback with ephemeral port
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0; // Let system choose port

    if (bind(listenfd, (struct sockaddr *)&sa, sa_len) < 0) {
        ESP_LOGE(TAG, "Failed to bind listening socket");
        close(listenfd);
        return -1;
    }

    if (listen(listenfd, 1) < 0) {
        ESP_LOGE(TAG, "Failed to listen");
        close(listenfd);
        return -1;
    }

    // Get the actual address/port we bound to
    if (getsockname(listenfd, (struct sockaddr *)&sa, &sa_len) < 0) {
        ESP_LOGE(TAG, "getsockname failed");
        close(listenfd);
        return -1;
    }

    // Create first socket and connect to listener
    fd1 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd1 == INVALID_SOCKET) {
        ESP_LOGE(TAG, "Cannot create fd1 socket");
        close(listenfd);
        return -1;
    }

    if (connect(fd1, (struct sockaddr *)&sa, sa_len) < 0) {
        ESP_LOGE(TAG, "Failed to connect fd1");
        close(listenfd);
        close(fd1);
        return -1;
    }

    // Accept connection to create second socket
    fd2 = accept(listenfd, NULL, 0);
    if (fd2 == INVALID_SOCKET) {
        ESP_LOGE(TAG, "Failed to accept fd2");
        close(listenfd);
        close(fd1);
        return -1;
    }

    close(listenfd);
    sv[0] = fd1;
    sv[1] = fd2;
    return 0;
}

// Implementation of pipe() using socketpair
extern "C" int pipe(int pipefd[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pipefd) == -1) {
        return -1;
    }

    // Make it unidirectional: pipefd[0] is read-only, pipefd[1] is write-only
    if (shutdown(pipefd[0], SHUT_WR) == -1 || shutdown(pipefd[1], SHUT_RD) == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    return 0;
}
