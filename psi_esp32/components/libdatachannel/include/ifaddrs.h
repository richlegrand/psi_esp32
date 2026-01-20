#pragma once

// ifaddrs compatibility header for ESP32
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

// Interface flags (from Linux)
#define IFF_UP          0x1     /* interface is up */
#define IFF_BROADCAST   0x2     /* broadcast address valid */
#define IFF_DEBUG       0x4     /* turn on debugging */
#define IFF_LOOPBACK    0x8     /* is a loopback net */
#define IFF_POINTOPOINT 0x10    /* interface is point-to-point link */
#define IFF_RUNNING     0x40    /* resources allocated */
#define IFF_NOARP       0x80    /* no address resolution protocol */
#define IFF_PROMISC     0x100   /* receive all packets */
#define IFF_ALLMULTI    0x200   /* receive all multicast packets */
#define IFF_MULTICAST   0x1000  /* supports multicast */

struct ifaddrs {
    struct ifaddrs  *ifa_next;    /* Next item in list */
    char            *ifa_name;    /* Name of interface */
    unsigned int     ifa_flags;   /* Flags from SIOCGIFFLAGS */
    struct sockaddr *ifa_addr;    /* Address of interface */
    struct sockaddr *ifa_netmask; /* Netmask of interface */
    union {
        struct sockaddr *ifu_broadaddr; /* Broadcast address of interface */
        struct sockaddr *ifu_dstaddr;   /* Point-to-point destination address */
    } ifa_ifu;
#define ifa_broadaddr ifa_ifu.ifu_broadaddr
#define ifa_dstaddr   ifa_ifu.ifu_dstaddr
    void            *ifa_data;    /* Address-specific data */
};

// Function declarations
int getifaddrs(struct ifaddrs **ifap);
void freeifaddrs(struct ifaddrs *ifa);

#ifdef __cplusplus
}
#endif